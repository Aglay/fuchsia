// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_DRIVERS_AUDIO_VIRTUAL_AUDIO_VIRTUAL_AUDIO_STREAM_H_
#define GARNET_DRIVERS_AUDIO_VIRTUAL_AUDIO_VIRTUAL_AUDIO_STREAM_H_

#include <ddk/debug.h>
#include <dispatcher-pool/dispatcher-timer.h>
#include <dispatcher-pool/dispatcher-wakeup-event.h>
#include <fbl/mutex.h>
#include <fbl/ref_ptr.h>
#include <lib/fzl/vmo-mapper.h>
#include <lib/simple-audio-stream/simple-audio-stream.h>
#include <lib/zx/vmo.h>
#include <deque>

#include "garnet/drivers/audio/virtual_audio/virtual_audio_device_impl.h"

namespace virtual_audio {

class VirtualAudioDeviceImpl;

class VirtualAudioStream : public ::audio::SimpleAudioStream {
 public:
  void ChangePlugState(bool plugged) __TA_EXCLUDES(msg_queue_lock_);

 protected:
  friend class ::audio::SimpleAudioStream;
  friend class fbl::RefPtr<VirtualAudioStream>;

  VirtualAudioStream(VirtualAudioDeviceImpl* parent, zx_device_t* dev_node,
                     bool is_input)
      : ::audio::SimpleAudioStream(dev_node, is_input), parent_(parent) {}
  ~VirtualAudioStream() override;

  zx_status_t Init() __TA_REQUIRES(domain_->token()) override;
  zx_status_t InitPost() override;

  zx_status_t ChangeFormat(const ::audio::audio_proto::StreamSetFmtReq& req)
      __TA_REQUIRES(domain_->token()) override;
  zx_status_t SetGain(const ::audio::audio_proto::SetGainReq& req)
      __TA_REQUIRES(domain_->token()) override;

  zx_status_t GetBuffer(const ::audio::audio_proto::RingBufGetBufferReq& req,
                        uint32_t* out_num_rb_frames, zx::vmo* out_buffer)
      __TA_REQUIRES(domain_->token()) override;

  zx_status_t Start(uint64_t* out_start_time)
      __TA_REQUIRES(domain_->token()) override;
  zx_status_t Stop() __TA_REQUIRES(domain_->token()) override;

  void ShutdownHook() __TA_REQUIRES(domain_->token()) override;
  // RingBufferShutdown() is unneeded: no hardware shutdown tasks needed...

  zx_status_t ProcessRingNotification() __TA_REQUIRES(domain_->token());

  enum class Msg { Plug, Unplug };
  void HandleMessages() __TA_REQUIRES(domain_->token())
      __TA_EXCLUDES(msg_queue_lock_);
  void HandleMsg(Msg msg) __TA_REQUIRES(domain_->token());

  // Accessed in GetBuffer, defended by token.
  fzl::VmoMapper ring_buffer_mapper_ __TA_GUARDED(domain_->token());
  uint32_t num_ring_buffer_frames_ __TA_GUARDED(domain_->token()) = 0;

  uint32_t max_buffer_frames_ __TA_GUARDED(domain_->token());
  uint32_t min_buffer_frames_ __TA_GUARDED(domain_->token());
  uint32_t modulo_buffer_frames_ __TA_GUARDED(domain_->token());

  fbl::RefPtr<dispatcher::Timer> notify_timer_;
  uint32_t us_per_notification_ __TA_GUARDED(domain_->token()) = 0;
  uint32_t notifications_per_ring_ __TA_GUARDED(domain_->token()) = 0;
  zx::time start_time_ __TA_GUARDED(domain_->token());

  uint32_t bytes_per_sec_ __TA_GUARDED(domain_->token()) = 0;
  uint32_t frame_rate_ __TA_GUARDED(domain_->token()) = 0;
  uint32_t num_channels_ __TA_GUARDED(domain_->token()) = 0;

  VirtualAudioDeviceImpl* parent_ __TA_GUARDED(domain_->token());

  fbl::RefPtr<::dispatcher::WakeupEvent> driver_wakeup_;
  fbl::Mutex msg_queue_lock_ __TA_ACQUIRED_AFTER(domain_->token());
  std::deque<Msg> msg_queue_ __TA_GUARDED(msg_queue_lock_);
};

}  // namespace virtual_audio

#endif  // GARNET_DRIVERS_AUDIO_VIRTUAL_AUDIO_VIRTUAL_AUDIO_STREAM_H_
