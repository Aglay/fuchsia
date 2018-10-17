// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/mediaplayer/fidl/buffer_set.h"

#include "garnet/bin/mediaplayer/graph/formatting.h"

namespace media_player {

// static
fbl::RefPtr<BufferSet> BufferSet::Create(
    const fuchsia::mediacodec::CodecPortBufferSettings& settings,
    uint64_t buffer_lifetime_ordinal, bool single_vmo) {
  return fbl::MakeRefCounted<BufferSet>(settings, buffer_lifetime_ordinal,
                                        single_vmo);
}

BufferSet::BufferSet(
    const fuchsia::mediacodec::CodecPortBufferSettings& settings,
    uint64_t buffer_lifetime_ordinal, bool single_vmo)
    : settings_(settings),
      single_vmo_(single_vmo),
      buffers_(settings_.packet_count_for_codec +
               settings_.packet_count_for_client) {
  free_buffer_count_ = buffers_.size();
  settings_.buffer_lifetime_ordinal = buffer_lifetime_ordinal;
}

BufferSet::~BufferSet() {
  // Release all the |PayloadBuffers| before |buffers_| is deleted.
  ReleaseAllDecoderOwnedBuffers();
}

fuchsia::mediacodec::CodecBuffer BufferSet::GetBufferDescriptor(
    uint32_t buffer_index, bool writeable,
    const PayloadVmos& payload_vmos) const {
  std::lock_guard<std::mutex> locker(mutex_);
  FXL_DCHECK(buffer_index < buffers_.size());

  fuchsia::mediacodec::CodecBuffer buffer;
  buffer.buffer_lifetime_ordinal = settings_.buffer_lifetime_ordinal;
  buffer.buffer_index = buffer_index;

  fbl::RefPtr<PayloadVmo> payload_vmo = BufferVmo(buffer_index, payload_vmos);
  FXL_DCHECK(payload_vmo);

  fuchsia::mediacodec::CodecBufferDataVmo buffer_data_vmo;
  buffer_data_vmo.vmo_handle = payload_vmo->Duplicate(
      ZX_RIGHT_READ | ZX_RIGHT_MAP | ZX_RIGHT_TRANSFER | ZX_RIGHT_DUPLICATE |
      (writeable ? ZX_RIGHT_WRITE : 0));
  buffer_data_vmo.vmo_usable_start =
      single_vmo_ ? (buffer_index * settings_.per_packet_buffer_bytes) : 0;
  buffer_data_vmo.vmo_usable_size = settings_.per_packet_buffer_bytes;

  buffer.data.set_vmo(std::move(buffer_data_vmo));

  return buffer;
}

fbl::RefPtr<PayloadBuffer> BufferSet::AllocateBuffer(
    uint64_t size, const PayloadVmos& payload_vmos) {
  std::lock_guard<std::mutex> locker(mutex_);
  FXL_DCHECK(size <= settings_.per_packet_buffer_bytes);
  FXL_DCHECK(free_buffer_count_ != 0);
  FXL_DCHECK(suggest_next_to_allocate_ < buffers_.size());

  std::vector<fbl::RefPtr<PayloadVmo>> vmos = payload_vmos.GetVmos();
  FXL_DCHECK(single_vmo_ ? (vmos.size() == 1)
                         : (vmos.size() == buffers_.size()));

  uint32_t index = suggest_next_to_allocate_;
  while (!buffers_[index].free_) {
    index = (index + 1) % buffers_.size();
    if (index == suggest_next_to_allocate_) {
      FXL_LOG(WARNING) << "AllocateBuffer: ran out of buffers";
      return nullptr;
    }
  }

  FXL_DCHECK(buffers_[index].decoder_ref_ == nullptr);
  FXL_DCHECK(buffers_[index].free_);
  buffers_[index].free_ = false;

  suggest_next_to_allocate_ = (index + 1) % buffers_.size();

  return CreateBuffer(index, vmos);
}

void BufferSet::CreateBufferForDecoder(uint32_t buffer_index,
                                       const PayloadVmos& payload_vmos) {
  std::lock_guard<std::mutex> locker(mutex_);
  FXL_DCHECK(buffer_index < buffers_.size());
  FXL_DCHECK(buffers_[buffer_index].free_);
  FXL_DCHECK(!buffers_[buffer_index].decoder_ref_);

  buffers_[buffer_index].free_ = false;
  buffers_[buffer_index].decoder_ref_ =
      CreateBuffer(buffer_index, payload_vmos.GetVmos());
}

void BufferSet::AddRefBufferForDecoder(
    uint32_t buffer_index, fbl::RefPtr<PayloadBuffer> payload_buffer) {
  FXL_DCHECK(payload_buffer);
  std::lock_guard<std::mutex> locker(mutex_);
  FXL_DCHECK(buffer_index < buffers_.size());
  FXL_DCHECK(!buffers_[buffer_index].free_);
  FXL_DCHECK(!buffers_[buffer_index].decoder_ref_);

  buffers_[buffer_index].decoder_ref_ = payload_buffer;
}

fbl::RefPtr<PayloadBuffer> BufferSet::TakeBufferFromDecoder(
    uint32_t buffer_index) {
  std::lock_guard<std::mutex> locker(mutex_);
  FXL_DCHECK(buffer_index < buffers_.size());
  FXL_DCHECK(!buffers_[buffer_index].free_);
  FXL_DCHECK(buffers_[buffer_index].decoder_ref_);

  auto result = buffers_[buffer_index].decoder_ref_;
  buffers_[buffer_index].decoder_ref_ = nullptr;

  return result;
}

void BufferSet::AllocateAllBuffersForDecoder(const PayloadVmos& payload_vmos) {
  std::lock_guard<std::mutex> locker(mutex_);

  for (uint32_t index = 0; index < buffers_.size(); ++index) {
    FXL_DCHECK(buffers_[index].free_);
    FXL_DCHECK(!buffers_[index].decoder_ref_);

    buffers_[index].free_ = false;
    buffers_[index].decoder_ref_ = CreateBuffer(index, payload_vmos.GetVmos());
  }

  free_buffer_count_ = 0;
}

void BufferSet::ReleaseAllDecoderOwnedBuffers() {
  std::vector<fbl::RefPtr<PayloadBuffer>> buffers_to_release_;

  {
    std::lock_guard<std::mutex> locker(mutex_);

    for (uint32_t index = 0; index < buffers_.size(); ++index) {
      if (buffers_[index].decoder_ref_) {
        buffers_to_release_.push_back(buffers_[index].decoder_ref_);
        buffers_[index].decoder_ref_ = nullptr;
      }
    }
  }

  // Buffers get released here (with the lock not taken) when
  // |buffers_to_release_| goes out of scope.
}

bool BufferSet::HasFreeBuffer(fit::closure callback) {
  std::lock_guard<std::mutex> locker(mutex_);
  if (free_buffer_count_ != 0) {
    return true;
  }

  free_buffer_callback_ = std::move(callback);

  return false;
}

void BufferSet::Decommission() {
  // This was probably taken care of by the decoder, but let's make sure. Any
  // decoder-owned buffers left behind will cause this |BufferSet| to leak.
  ReleaseAllDecoderOwnedBuffers();

  std::lock_guard<std::mutex> locker(mutex_);
  free_buffer_callback_ = nullptr;
}

fbl::RefPtr<PayloadVmo> BufferSet::BufferVmo(
    size_t buffer_index, const PayloadVmos& payload_vmos) const {
  FXL_DCHECK(buffer_index < buffers_.size());

  const std::vector<fbl::RefPtr<PayloadVmo>>& vmos = payload_vmos.GetVmos();
  if (single_vmo_) {
    FXL_DCHECK(vmos.size() == 1);
    return vmos[0];
  } else {
    FXL_DCHECK(vmos.size() >= buffers_.size());
    return vmos[buffer_index];
  }
}

fbl::RefPtr<PayloadBuffer> BufferSet::CreateBuffer(
    uint32_t buffer_index,
    const std::vector<fbl::RefPtr<PayloadVmo>>& payload_vmos) {
  fbl::RefPtr<PayloadVmo> payload_vmo =
      (single_vmo_ ? payload_vmos[0] : payload_vmos[buffer_index]);
  uint64_t offset_in_vmo =
      single_vmo_ ? buffer_index * settings_.per_packet_buffer_bytes : 0;

  // The recycler used here captures an |fbl::RefPtr| to |this| in case this
  // buffer set is no longer current when the buffer is recycled.
  fbl::RefPtr<PayloadBuffer> payload_buffer = PayloadBuffer::Create(
      settings_.per_packet_buffer_bytes, payload_vmo->at_offset(offset_in_vmo),
      payload_vmo, offset_in_vmo,
      [this, buffer_index,
       this_ref = fbl::WrapRefPtr(this)](PayloadBuffer* payload_buffer) {
        fit::closure free_buffer_callback;

        {
          std::lock_guard<std::mutex> locker(mutex_);
          FXL_DCHECK(buffer_index < buffers_.size());
          FXL_DCHECK(!buffers_[buffer_index].free_);
          FXL_DCHECK(!buffers_[buffer_index].decoder_ref_);

          buffers_[buffer_index].free_ = true;
          ++free_buffer_count_;

          free_buffer_callback = std::move(free_buffer_callback_);
        }

        if (free_buffer_callback) {
          free_buffer_callback();
        }
      });

  payload_buffer->SetId(buffer_index);
  payload_buffer->SetBufferConfig(settings_.buffer_lifetime_ordinal);
  --free_buffer_count_;

  return payload_buffer;
}

void BufferSetManager::ApplyConstraints(
    const fuchsia::mediacodec::CodecBufferConstraints& constraints,
    bool prefer_single_vmo) {
  FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);

  uint64_t lifetime_ordinal = 1;

  if (current_set_) {
    lifetime_ordinal = current_set_->lifetime_ordinal() + 2;
    current_set_->Decommission();
  }

  current_set_ = BufferSet::Create(
      constraints.default_settings, lifetime_ordinal,
      prefer_single_vmo && constraints.single_buffer_mode_allowed);
}

void BufferSetManager::ReleaseBufferForDecoder(uint64_t lifetime_ordinal,
                                               uint32_t index) {
  FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);

  if (current_set_ && lifetime_ordinal == current_set_->lifetime_ordinal()) {
    // Release the buffer from the current set.
    current_set_->TakeBufferFromDecoder(index);
    return;
  }

  // The buffer is from an old set and has already been released for the
  // decoder.
}

}  // namespace media_player
