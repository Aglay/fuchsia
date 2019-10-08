// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_AUDIO_CORE_AUDIO_OBJECT_H_
#define SRC_MEDIA_AUDIO_AUDIO_CORE_AUDIO_OBJECT_H_

#include <lib/fit/function.h>

#include <fbl/ref_counted.h>
#include <fbl/ref_ptr.h>

#include "src/lib/fxl/synchronization/thread_annotations.h"
#include "src/media/audio/audio_core/audio_link.h"
#include "src/media/audio/audio_core/audio_renderer_format_info.h"
#include "src/media/audio/audio_core/volume_curve.h"

namespace media::audio {

// The simple base class for 4 major types of audio objects in the mixer: Outputs, Inputs,
// AudioRenderers and AudioCapturers. It ensures that each is intrusively ref-counted, and remembers
// its type so that it may be safely downcast from generic object to something more specific.
class AudioObject : public fbl::RefCounted<AudioObject> {
 public:
  // Disallow copy, assign, and move.
  AudioObject& operator=(AudioObject) = delete;
  AudioObject(const AudioObject&) = delete;
  AudioObject(AudioObject&&) = delete;
  AudioObject& operator=(AudioObject&&) = delete;

  enum class Type {
    Output,
    Input,
    AudioRenderer,
    AudioCapturer,
  };

  static fbl::RefPtr<AudioLink> LinkObjects(const fbl::RefPtr<AudioObject>& source,
                                            const fbl::RefPtr<AudioObject>& dest);
  static void RemoveLink(const fbl::RefPtr<AudioLink>& link);

  void UnlinkSources();
  void UnlinkDestinations();
  void Unlink() {
    UnlinkSources();
    UnlinkDestinations();
  }

  // PreventNewLinks
  //
  // Clears 'new_links_allowed' from within the links_lock, ensuring no further links are added to
  // this object. This call is one of the first steps in the shutdown process of an AudioObject.
  //
  // TODO(johngro): Consider eliminating. Given how links are created/destroyed, we may not need it.
  void PreventNewLinks() {
    std::lock_guard<std::mutex> lock(links_lock_);
    new_links_allowed_ = false;
  }

  // The VolumeCurve for the object, representing its mapping from volume to gain.
  virtual std::optional<VolumeCurve> GetVolumeCurve() const { return std::nullopt; }

  // Note: format_info() is subject to change and must only be accessed from the main message loop
  // thread. Outputs which are running on mixer threads should never access format_info() directly
  // from a mix thread. Instead, they should use the format_info which was assigned to the AudioLink
  // at the time the link was created.
  virtual const fbl::RefPtr<AudioRendererFormatInfo>& format_info() const {
    static fbl::RefPtr<AudioRendererFormatInfo> null_info;
    return null_info;
  }

  bool format_info_valid() const { return format_info() != nullptr; }

  virtual std::optional<std::pair<TimelineFunction, uint32_t>> SnapshotCurrentTimelineFunction(
      int64_t reference_time) {
    return std::nullopt;
  }

  // Hooks to add logging or metrics for [Partial]Underflow events.
  virtual void UnderflowOccurred(int64_t source_start, int64_t mix_point,
                                 zx_duration_t underflow_duration) {}
  virtual void PartialUnderflowOccurred(int64_t source_offset, int64_t mix_offset) {}

  Type type() const { return type_; }
  bool is_output() const { return type() == Type::Output; }
  bool is_input() const { return type() == Type::Input; }
  bool is_audio_renderer() const { return type() == Type::AudioRenderer; }
  bool is_audio_capturer() const { return type() == Type::AudioCapturer; }

 protected:
  friend class fbl::RefPtr<AudioObject>;
  explicit AudioObject(Type type) : type_(type) {}
  virtual ~AudioObject() {}

  // Initialize(Source|Dest)Link
  //
  // Called on the AudioCore's main message loop any time a source and a
  // destination are being linked via AudioObject::LinkObjects. By default,
  // these hooks do nothing, but AudioObject subclasses may use them to set the
  // properties of a link (or reject the link) before the link gets added to the
  // source and destination link sets.
  //
  // For example, Sources like an AudioRenderer override InitializeDestLink in
  // order to set the source gain and to make a copy of their pending packet
  // queue. Destinations like an output override InitializeSourceLink in order
  // to choose and initialize an appropriate resampling filter.
  //
  // @return MediaResult::OK if initialization succeeded, or an appropriate
  // error code otherwise.
  virtual zx_status_t InitializeSourceLink(const fbl::RefPtr<AudioLink>& link);
  virtual zx_status_t InitializeDestLink(const fbl::RefPtr<AudioLink>& link);

  // Called immediately after a new link is added to the object.
  virtual void OnLinkAdded();

  std::mutex links_lock_;

  // The set of links which this audio device is acting as a source for (eg; the
  // destinations that this object is sending to). The target of each of these
  // links must be a either an Output or a AudioCapturer.
  typename AudioLink::Set<AudioLink::Dest> dest_links_ FXL_GUARDED_BY(links_lock_);

  // The set of links which this audio device is acting as a destination for
  // (eg; the sources that that the object is receiving from). The target of
  // each of these links must be a either an Output or a AudioCapturer.
  //
  // TODO(johngro): Order this by priority. Use a fbl::WAVLTree (or some other
  // form of ordered intrusive container) so that we can easily remove and
  // re-insert a link if/when priority changes.
  //
  // Right now, we have no priorities, so this is just a set of
  // AudioRenderer/output links.
  typename AudioLink::Set<AudioLink::Source> source_links_ FXL_GUARDED_BY(links_lock_);

  // The following iterator functions accept a function (see below) and call it
  // sequentially with each destination link as a parameter. As described below,
  // depending on which iterator is used, either every link is guaranteed to be
  // included, or iteration will terminate early as soon as a task returns true.
  //
  // This iterator approach reduces our ability to use static thread analysis
  // effectively, so use with care. ForEachDestLink and ForAnyDestLink each
  // obtain the links_lock_ and hold it while each LinkFunction or
  // LinkBoolFunction is invoked. For this reason,
  //    1) Callers into the ForEachSourceLink, ForEachDestLink or ForAnyDestLink
  //           functions must not already hold links_lock_; additionally,
  //    2) A LinkFunction or LinkBoolFunction must not
  //        a) attempt to obtain links_lock_ directly, nor
  //        b) acquire any lock marked as acquired_before(links_lock_), nor
  //        c) call any function which excludes links_lock_.
  //

  // The inline_functions below reserve stack space for up to four pointers.
  // This can be increased as needed (but should NOT be needed any time soon).
  //
  // LinkFunction has no return value and is used with ForEach[Source|Dest]Link.
  using LinkFunction = fit::inline_function<void(AudioLink& link), sizeof(void*) * 4>;
  // Same as LinkFunction, but returns bool for early termination. This
  // return val is used by ForAnyDestLink (or a future ForAllDestLinks).
  // Currently stack space for one ptr is provided (the one caller needs 0).
  using LinkBoolFunction = fit::inline_function<bool(AudioLink& link), sizeof(void*) * 1>;

  // Link Iterators - these functions iterate upon LinkPacketSource types only.
  //
  // Run this task on AudioLinks in source_links_. All links will be called.
  void ForEachSourceLink(const LinkFunction& source_task) FXL_LOCKS_EXCLUDED(links_lock_);

  // Run this task on every AudioLink in dest_links_. All links will be called.
  void ForEachDestLink(const LinkFunction& dest_task) FXL_LOCKS_EXCLUDED(links_lock_);

  // Run this task on each dest link. If any returns 'true', ForAnyDestLink
  // immediately returns 'true' without calling the remaining links. If none
  // returns 'true' or if link set is empty, ForAnyDestLink returns 'false'.
  bool ForAnyDestLink(const LinkBoolFunction& dest_task) FXL_LOCKS_EXCLUDED(links_lock_);

 private:
  template <typename TagType>
  void UnlinkCleanup(typename AudioLink::Set<TagType>* links);

  const Type type_;
  bool new_links_allowed_ FXL_GUARDED_BY(links_lock_) = true;
};

}  // namespace media::audio

#endif  // SRC_MEDIA_AUDIO_AUDIO_CORE_AUDIO_OBJECT_H_
