// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/audio_core/audio_object.h"

#include <trace/event.h>

#include "src/lib/fxl/logging.h"
#include "src/media/audio/audio_core/audio_device.h"
#include "src/media/audio/audio_core/audio_link.h"
#include "src/media/audio/audio_core/audio_link_packet_source.h"
#include "src/media/audio/audio_core/audio_link_ring_buffer_source.h"
#include "src/media/audio/audio_core/audio_renderer_impl.h"

namespace media::audio {

// static
fbl::RefPtr<AudioLink> AudioObject::LinkObjects(const fbl::RefPtr<AudioObject>& source,
                                                const fbl::RefPtr<AudioObject>& dest) {
  TRACE_DURATION("audio", "AudioObject::LinkObjects");
  // Assert this source is valid (AudioCapturers are disallowed).
  FXL_DCHECK(source != nullptr);
  FXL_DCHECK((source->type() == AudioObject::Type::AudioRenderer) ||
             (source->type() == AudioObject::Type::Output) ||
             (source->type() == AudioObject::Type::Input));

  // Assert this destination is valid (inputs and AudioRenderers disallowed).
  FXL_DCHECK(dest != nullptr);
  FXL_DCHECK((dest->type() == AudioObject::Type::Output) ||
             (dest->type() == AudioObject::Type::AudioCapturer));

  // Assert that we are not connecting looped-back-output to output.
  FXL_DCHECK((source->type() != AudioObject::Type::Output) ||
             (dest->type() != AudioObject::Type::Output));

  // Create a link of the appropriate type based on our source.
  fbl::RefPtr<AudioLink> link;
  if (source->type() == AudioObject::Type::AudioRenderer) {
    FXL_DCHECK(source->format_info_valid());
    link = AudioLinkPacketSource::Create(source, dest, source->format_info());
  } else {
    link = AudioLinkRingBufferSource::Create(source, dest);
  }

  // Give source and destination a chance to initialize (or reject) the link.
  zx_status_t res;
  res = source->InitializeDestLink(link);
  if (res != ZX_OK) {
    return nullptr;
  }
  res = dest->InitializeSourceLink(link);
  if (res != ZX_OK) {
    return nullptr;
  }

  // Now lock both objects then add the link to the proper sets in both source and destination.
  {
    std::lock_guard<std::mutex> slock(source->links_lock_);
    std::lock_guard<std::mutex> dlock(dest->links_lock_);
    source->dest_links_.insert(link);
    dest->source_links_.insert(link);
  }

  source->OnLinkAdded();
  dest->OnLinkAdded();

  return link;
}

// static
void AudioObject::RemoveLink(const fbl::RefPtr<AudioLink>& link) {
  TRACE_DURATION("audio", "AudioObject::RemoveLink");
  FXL_DCHECK(link != nullptr);

  link->Invalidate();

  const fbl::RefPtr<AudioObject>& source = link->GetSource();
  FXL_DCHECK(source != nullptr);
  {
    std::lock_guard<std::mutex> slock(source->links_lock_);
    auto iter = source->dest_links_.find(link.get());
    if (iter != source->dest_links_.end()) {
      source->dest_links_.erase(iter);
    }
  }

  const fbl::RefPtr<AudioObject>& dest = link->GetDest();
  FXL_DCHECK(dest != nullptr);
  {
    std::lock_guard<std::mutex> dlock(dest->links_lock_);
    auto iter = dest->source_links_.find(link.get());
    if (iter != dest->source_links_.end()) {
      dest->source_links_.erase(iter);
    }
  }
}

// Call the provided function for each source link (passing the link as param). This distributes
// calls such as SetGain to every AudioCapturer path.
void AudioObject::ForEachSourceLink(const LinkFunction& source_task) {
  TRACE_DURATION("audio", "AudioObject::ForEachSourceLink");
  std::lock_guard<std::mutex> links_lock(links_lock_);

  // Callers (generally AudioCapturers) should never be linked to destinations.
  FXL_DCHECK(dest_links_.is_empty());

  for (auto& link : source_links_) {
    source_task(link);
  }
}

// Call the provided function for each dest link (passing the link as a param). This distributes
// calls such as SetGain to every AudioRenderer output path.
void AudioObject::ForEachDestLink(const LinkFunction& dest_task) {
  TRACE_DURATION("audio", "AudioObject::ForEachDestLink");
  std::lock_guard<std::mutex> links_lock(links_lock_);

  // Callers (generally AudioRenderers) should never be linked to sources.
  FXL_DCHECK(source_links_.is_empty());

  for (auto& link : dest_links_) {
    dest_task(link);
  }
}

// Call the provided function for each destination link, until one returns true.
bool AudioObject::ForAnyDestLink(const LinkBoolFunction& dest_task) {
  TRACE_DURATION("audio", "AudioObject::ForAnyDestLink");
  std::lock_guard<std::mutex> links_lock(links_lock_);

  FXL_DCHECK(source_links_.is_empty());

  for (auto& link : dest_links_) {
    if (dest_task(link)) {
      return true;  // This link satisfied the need; we are done.
    }
    // Else, continue inquiring with the remaining links.
  }
  return false;  // No link satisfied the need.
}

void AudioObject::UnlinkSources() {
  TRACE_DURATION("audio", "AudioObject::UnlinkSources");
  typename AudioLink::Set<AudioLink::Source> old_links;
  {
    std::lock_guard<std::mutex> lock(links_lock_);
    old_links = std::move(source_links_);
  }
  UnlinkCleanup<AudioLink::Source>(&old_links);
}

void AudioObject::UnlinkDestinations() {
  TRACE_DURATION("audio", "AudioObject::UnlinkDestinations");
  typename AudioLink::Set<AudioLink::Dest> old_links;
  {
    std::lock_guard<std::mutex> lock(links_lock_);
    old_links = std::move(dest_links_);
  }
  UnlinkCleanup<AudioLink::Dest>(&old_links);
}

zx_status_t AudioObject::InitializeSourceLink(const fbl::RefPtr<AudioLink>&) { return ZX_OK; }

zx_status_t AudioObject::InitializeDestLink(const fbl::RefPtr<AudioLink>&) { return ZX_OK; }

void AudioObject::OnLinkAdded() {}

template <typename TagType>
void AudioObject::UnlinkCleanup(typename AudioLink::Set<TagType>* links) {
  TRACE_DURATION("audio", "AudioObject::UnlinkCleanup");
  FXL_DCHECK(links != nullptr);

  // Note: we could just range-based for-loop over this set and call RemoveLink on each member.
  // Instead, we remove each element from our local set before calling RemoveLinks. This will make a
  // future transition to intrusive containers a bit easier. Explanations available on request.
  while (!links->is_empty()) {
    auto link = links->pop_front();
    RemoveLink(link);
    link = nullptr;
  }
}

}  // namespace media::audio
