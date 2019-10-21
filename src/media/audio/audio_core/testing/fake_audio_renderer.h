// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_AUDIO_CORE_TESTING_FAKE_AUDIO_RENDERER_H_
#define SRC_MEDIA_AUDIO_AUDIO_CORE_TESTING_FAKE_AUDIO_RENDERER_H_

#include <fuchsia/media/cpp/fidl.h>
#include <lib/zx/time.h>

#include <fbl/ref_ptr.h>

#include "src/media/audio/audio_core/audio_object.h"
#include "src/media/audio/audio_core/utils.h"

namespace media::audio::testing {

class FakeAudioRenderer : public AudioObject, public fuchsia::media::AudioRenderer {
 public:
  static fbl::RefPtr<FakeAudioRenderer> Create(async_dispatcher_t* dispatcher) {
    return fbl::AdoptRef(new FakeAudioRenderer(dispatcher));
  }
  static fbl::RefPtr<FakeAudioRenderer> CreateWithDefaultFormatInfo(async_dispatcher_t* dispatcher);

  FakeAudioRenderer(async_dispatcher_t* dispatcher);

  void set_format_info(fbl::RefPtr<AudioRendererFormatInfo> format_info) {
    format_info_ = std::move(format_info);
  }

  // Enqueues a packet that has all samples initialized to |sample| and lasts for |duration|.
  void EnqueueAudioPacket(float sample, zx::duration duration = zx::msec(1));

  // |media::audio::AudioObject|
  const fbl::RefPtr<AudioRendererFormatInfo>& format_info() const override { return format_info_; }
  std::optional<std::pair<TimelineFunction, uint32_t>> SnapshotCurrentTimelineFunction(
      int64_t reference_time) override {
    return {std::make_pair(timeline_func_, 1)};
  }

  // |fuchsia::media::AudioRenderer|
  void AddPayloadBuffer(uint32_t id, ::zx::vmo payload_buffer) override {}
  void RemovePayloadBuffer(uint32_t id) override {}
  void SendPacket(fuchsia::media::StreamPacket packet, SendPacketCallback callback) override {}
  void SendPacketNoReply(fuchsia::media::StreamPacket packet) override {}
  void EndOfStream() override {}
  void DiscardAllPackets(DiscardAllPacketsCallback callback) override {}
  void DiscardAllPacketsNoReply() override {}
  void SetPcmStreamType(fuchsia::media::AudioStreamType type) override {}
  void SetPtsUnits(uint32_t tick_per_second_numerator,
                   uint32_t tick_per_second_denominator) override {}
  void SetPtsContinuityThreshold(float threshold_seconds) override {}
  void SetReferenceClock(::zx::handle reference_clock) override {}
  void Play(int64_t reference_time, int64_t media_time, PlayCallback callback) override {}
  void PlayNoReply(int64_t reference_time, int64_t media_time) override {}
  void Pause(PauseCallback callback) override {}
  void PauseNoReply() override {}
  void EnableMinLeadTimeEvents(bool enabled) override {}
  void GetMinLeadTime(GetMinLeadTimeCallback callback) override {}
  void BindGainControl(::fidl::InterfaceRequest<::fuchsia::media::audio::GainControl>
                           gain_control_request) override {}
  void SetUsage(fuchsia::media::AudioRenderUsage usage) override {}

 private:
  zx::duration FindMinLeadTime();

  async_dispatcher_t* dispatcher_;
  fbl::RefPtr<AudioRendererFormatInfo> format_info_ = nullptr;
  fbl::RefPtr<RefCountedVmoMapper> vmo_ref_;
  size_t buffer_offset_ = 0;
  TimelineFunction timeline_func_;
  uint32_t next_pts_ = 0;
};

}  // namespace media::audio::testing

#endif  // SRC_MEDIA_AUDIO_AUDIO_CORE_TESTING_FAKE_AUDIO_RENDERER_H_
