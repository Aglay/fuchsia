// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/audio_core/audio_packet_ref.h"

#include <lib/async/cpp/task.h>

#include <trace/event.h>

#include "src/lib/fxl/logging.h"

namespace media::audio {

AudioPacketRef::AudioPacketRef(fbl::RefPtr<RefCountedVmoMapper> vmo_ref,
                               async_dispatcher_t* callback_dispatcher,
                               fuchsia::media::AudioRenderer::SendPacketCallback callback,
                               fuchsia::media::StreamPacket packet, uint32_t frac_frame_len,
                               int64_t start_pts)
    : vmo_ref_(std::move(vmo_ref)),
      callback_(std::move(callback)),
      packet_(packet),
      frac_frame_len_(frac_frame_len),
      start_pts_(start_pts),
      end_pts_(start_pts + frac_frame_len),
      dispatcher_(callback_dispatcher) {
  TRACE_DURATION("audio", "AudioPacketRef::AudioPacketRef");
  TRACE_FLOW_BEGIN("audio.debug", "process_packet", nonce_);
  FXL_DCHECK(dispatcher_ != nullptr);
  FXL_DCHECK(vmo_ref_ != nullptr);
}

void AudioPacketRef::fbl_recycle() {
  TRACE_DURATION("audio", "AudioPacketRef::fbl_recycle");
  TRACE_FLOW_END("audio.debug", "process_packet", nonce_);

  if (callback_) {
    async::PostTask(dispatcher_, std::move(callback_));
  }

  delete this;
}

}  // namespace media::audio
