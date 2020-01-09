// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/audio_core/audio_output.h"

#include "src/media/audio/audio_core/testing/fake_audio_renderer.h"
#include "src/media/audio/audio_core/testing/stub_device_registry.h"
#include "src/media/audio/audio_core/testing/threading_model_fixture.h"

namespace media::audio {
namespace {

class TestAudioOutput : public AudioOutput {
 public:
  TestAudioOutput(ThreadingModel* threading_model, DeviceRegistry* registry)
      : AudioOutput(threading_model, registry) {}

  using AudioOutput::SetNextSchedTime;
  void SetupMixTask(const Format& format, uint32_t max_frames) {
    OBTAIN_EXECUTION_DOMAIN_TOKEN(token, &mix_domain());
    AudioOutput::SetupMixTask(format, max_frames);
  }
  void Process() {
    OBTAIN_EXECUTION_DOMAIN_TOKEN(token, &mix_domain());
    AudioOutput::Process();
  }

  // Allow a test to provide a delegate to handle |AudioOutput::StartMixJob| invocations.
  using StartMixDelegate = fit::function<std::optional<MixStage::FrameSpan>(zx::time)>;
  void set_start_mix_delegate(StartMixDelegate delegate) {
    start_mix_delegate_ = std::move(delegate);
  }

  // Allow a test to provide a delegate to handle |AudioOutput::FinishMixJob| invocations.
  using FinishMixDelegate = fit::function<void(const MixStage::FrameSpan&, float* buffer)>;
  void set_finish_mix_delegate(FinishMixDelegate delegate) {
    finish_mix_delegate_ = std::move(delegate);
  }

  // |AudioOutput|
  std::optional<MixStage::FrameSpan> StartMixJob(zx::time process_start) override {
    if (start_mix_delegate_) {
      return start_mix_delegate_(process_start);
    } else {
      return std::nullopt;
    }
  }
  void FinishMixJob(const MixStage::FrameSpan& span, float* buffer) {
    if (finish_mix_delegate_) {
      finish_mix_delegate_(span, buffer);
    }
  }
  // |AudioDevice|
  void ApplyGainLimits(fuchsia::media::AudioGainInfo* in_out_info, uint32_t set_flags) override {}
  void OnWakeup() {}

 private:
  StartMixDelegate start_mix_delegate_;
  FinishMixDelegate finish_mix_delegate_;
};

class AudioOutputTest : public testing::ThreadingModelFixture {
 protected:
  testing::StubDeviceRegistry device_registry_;
  fbl::RefPtr<TestAudioOutput> audio_output_ =
      fbl::MakeRefCounted<TestAudioOutput>(&threading_model(), &device_registry_);
};

TEST_F(AudioOutputTest, ProcessTrimsInputStreamsIfNoMixJobProvided) {
  auto renderer = testing::FakeAudioRenderer::CreateWithDefaultFormatInfo(dispatcher());
  audio_output_->SetupMixTask(renderer->format()->stream_type(), zx::msec(1).to_msecs());
  AudioObject::LinkObjects(renderer, audio_output_);

  // StartMixJob always returns nullopt (no work) and schedules another mix 1ms in the future.
  audio_output_->set_start_mix_delegate([this, audio_output = audio_output_.get()](zx::time now) {
    audio_output->SetNextSchedTime(Now() + zx::msec(1));
    return std::nullopt;
  });

  // Enqueue 2 packets:
  //   * packet 1 from 0ms -> 5ms.
  //   * packet 2 from 5ms -> 10ms.
  bool packet1_released = false;
  bool packet2_released = false;
  renderer->EnqueueAudioPacket(1.0, zx::msec(5), [&packet1_released] {
    FX_LOGS(ERROR) << "Release packet 1";
    packet1_released = true;
  });
  renderer->EnqueueAudioPacket(1.0, zx::msec(5), [&packet2_released] {
    FX_LOGS(ERROR) << "Release packet 2";
    packet2_released = true;
  });

  // Process kicks off the periodic mix task.
  audio_output_->Process();

  // After 4ms we should still be retaining packet1.
  RunLoopFor(zx::msec(4));
  ASSERT_FALSE(packet1_released);

  // 5ms; all the audio from packet1 is consumed and it should be released. We should still have
  // packet2, however.
  RunLoopFor(zx::msec(1));
  ASSERT_TRUE(packet1_released && !packet2_released);

  // After 9ms we should still be retaining packet2.
  RunLoopFor(zx::msec(4));
  ASSERT_FALSE(packet2_released);

  // Finally after 10ms we will have released packet2.
  RunLoopFor(zx::msec(1));
  ASSERT_TRUE(packet2_released);
}

}  // namespace
}  // namespace media::audio
