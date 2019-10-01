// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/audio_core/ring_buffer.h"

#include <trace/event.h>

#include "src/lib/fxl/logging.h"

namespace media::audio {

// static
fbl::RefPtr<RingBuffer> RingBuffer::Create(zx::vmo vmo, uint32_t frame_size, uint32_t frame_count,
                                           bool input) {
  TRACE_DURATION("audio", "RingBuffer::Create");
  auto ret = fbl::AdoptRef(new RingBuffer());

  if (ret->Init(std::move(vmo), frame_size, frame_count, input) != ZX_OK) {
    return nullptr;
  }

  return ret;
}

zx_status_t RingBuffer::Init(zx::vmo vmo, uint32_t frame_size, uint32_t frame_count, bool input) {
  TRACE_DURATION("audio", "RingBuffer::Init");
  if (!vmo.is_valid()) {
    FXL_LOG(ERROR) << "Invalid VMO!";
    return ZX_ERR_INVALID_ARGS;
  }

  if (!frame_size) {
    FXL_LOG(ERROR) << "Frame size may not be zero!";
    return ZX_ERR_INVALID_ARGS;
  }

  uint64_t vmo_size;
  zx_status_t res = vmo.get_size(&vmo_size);

  if (res != ZX_OK) {
    FXL_PLOG(ERROR, res) << "Failed to get ring buffer VMO size";
    return res;
  }

  uint64_t size = static_cast<uint64_t>(frame_size) * frame_count;
  if (size > vmo_size) {
    FXL_LOG(ERROR) << "Driver-reported ring buffer size (" << size << ") is greater than VMO size ("
                   << vmo_size << ")";
    return res;
  }

  // Map the VMO into our address space.
  // TODO(35022): How do I specify the cache policy for this mapping?
  zx_vm_option_t flags = ZX_VM_PERM_READ | (input ? 0 : ZX_VM_PERM_WRITE);
  res = vmo_mapper_.Map(vmo, 0u, size, flags);

  if (res != ZX_OK) {
    FXL_PLOG(ERROR, res) << "Failed to map ring buffer VMO";
    return res;
  }

  frame_size_ = frame_size;
  frames_ = frame_count;

  return res;
}

}  // namespace media::audio
