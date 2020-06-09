// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/audio_core/audio_output.h"

#include "src/media/audio/audio_core/audio_device_manager.h"
#include "src/media/audio/audio_core/loudness_transform.h"
#include "src/media/audio/audio_core/testing/fake_audio_renderer.h"
#include "src/media/audio/audio_core/testing/threading_model_fixture.h"

namespace media::audio {
namespace {

// An OutputPipeline that always returns std::nullopt from |ReadLock|.
class TestOutputPipeline : public OutputPipeline {
 public:
  TestOutputPipeline(const Format& format) : OutputPipeline(format) {}

  void Enqueue(ReadableStream::Buffer buffer) { buffers_.push_back(std::move(buffer)); }

  // |media::audio::ReadableStream|
  std::optional<ReadableStream::Buffer> ReadLock(zx::time ref_time, int64_t frame,
                                                 uint32_t frame_count) override {
    if (buffers_.empty()) {
      return std::nullopt;
    }
    auto buffer = std::move(buffers_.front());
    buffers_.pop_front();
    return buffer;
  }
  void Trim(zx::time trim_threshold) override {}
  TimelineFunctionSnapshot ReferenceClockToFractionalFrames() const override {
    return TimelineFunctionSnapshot{
        .timeline_function = TimelineFunction(),
        .generation = kInvalidGenerationId,
    };
  }

  // |media::audio::OutputPipeline|
  std::shared_ptr<ReadableStream> loopback() const override { return nullptr; }
  std::shared_ptr<Mixer> AddInput(
      std::shared_ptr<ReadableStream> stream, const StreamUsage& usage,
      Mixer::Resampler sampler_hint = Mixer::Resampler::Default) override {
    return nullptr;
  }
  void RemoveInput(const ReadableStream& stream) override {}
  fit::result<void, fuchsia::media::audio::UpdateEffectError> UpdateEffect(
      const std::string& instance_name, const std::string& config) override {
    return fit::error(fuchsia::media::audio::UpdateEffectError::NOT_FOUND);
  }

 private:
  std::deque<ReadableStream::Buffer> buffers_;
};

class TestAudioOutput : public AudioOutput {
 public:
  TestAudioOutput(ThreadingModel* threading_model, DeviceRegistry* registry,
                  LinkMatrix* link_matrix)
      : AudioOutput(threading_model, registry, link_matrix) {}

  using AudioOutput::FrameSpan;
  using AudioOutput::SetNextSchedTime;
  void SetupMixTask(const PipelineConfig& config, const VolumeCurve& volume_curve,
                    uint32_t channels, uint32_t max_frames,
                    TimelineFunction clock_mono_to_output_frame) {
    OBTAIN_EXECUTION_DOMAIN_TOKEN(token, &mix_domain());
    AudioOutput::SetupMixTask(config, volume_curve, channels, max_frames,
                              clock_mono_to_output_frame);
  }
  void Process() {
    OBTAIN_EXECUTION_DOMAIN_TOKEN(token, &mix_domain());
    AudioOutput::Process();
  }
  std::unique_ptr<OutputPipeline> CreateOutputPipeline(
      const PipelineConfig& config, const VolumeCurve& volume_curve, uint32_t channels,
      size_t max_block_size_frames,
      TimelineFunction device_reference_clock_to_fractional_frame) override {
    if (output_pipeline_) {
      return std::move(output_pipeline_);
    }
    return AudioOutput::CreateOutputPipeline(config, volume_curve, channels, max_block_size_frames,
                                             device_reference_clock_to_fractional_frame);
  }

  // Allow a test to provide a delegate to handle |AudioOutput::StartMixJob| invocations.
  using StartMixDelegate = fit::function<std::optional<AudioOutput::FrameSpan>(zx::time)>;
  void set_start_mix_delegate(StartMixDelegate delegate) {
    start_mix_delegate_ = std::move(delegate);
  }

  // Allow a test to provide a delegate to handle |AudioOutput::FinishMixJob| invocations.
  using FinishMixDelegate = fit::function<void(const AudioOutput::FrameSpan&, float* buffer)>;
  void set_finish_mix_delegate(FinishMixDelegate delegate) {
    finish_mix_delegate_ = std::move(delegate);
  }
  void set_output_pipeline(std::unique_ptr<OutputPipeline> output_pipeline) {
    output_pipeline_ = std::move(output_pipeline);
  }

  // |AudioOutput|
  std::optional<AudioOutput::FrameSpan> StartMixJob(zx::time process_start) override {
    if (start_mix_delegate_) {
      return start_mix_delegate_(process_start);
    } else {
      return std::nullopt;
    }
  }
  void FinishMixJob(const AudioOutput::FrameSpan& span, float* buffer) override {
    if (finish_mix_delegate_) {
      finish_mix_delegate_(span, buffer);
    }
  }
  // |AudioDevice|
  void ApplyGainLimits(fuchsia::media::AudioGainInfo* in_out_info,
                       fuchsia::media::AudioGainValidFlags set_flags) override {}
  void OnWakeup() {}

 private:
  StartMixDelegate start_mix_delegate_;
  FinishMixDelegate finish_mix_delegate_;
  std::unique_ptr<OutputPipeline> output_pipeline_;
};

class AudioOutputTest : public testing::ThreadingModelFixture {
 protected:
  VolumeCurve volume_curve_ = VolumeCurve::DefaultForMinGain(Gain::kMinGainDb);
  std::shared_ptr<TestAudioOutput> audio_output_ = std::make_shared<TestAudioOutput>(
      &threading_model(), &context().device_manager(), &context().link_matrix());
};

TEST_F(AudioOutputTest, ProcessTrimsInputStreamsIfNoMixJobProvided) {
  auto renderer = testing::FakeAudioRenderer::CreateWithDefaultFormatInfo(dispatcher(),
                                                                          &context().link_matrix());
  static const TimelineFunction kOneFramePerMs = TimelineFunction(TimelineRate(1, 1'000'000));
  static PipelineConfig config = PipelineConfig::Default();
  audio_output_->SetupMixTask(config, volume_curve_, renderer->format()->channels(),
                              zx::msec(1).to_msecs(), kOneFramePerMs);
  context().link_matrix().LinkObjects(renderer, audio_output_,
                                      std::make_shared<MappedLoudnessTransform>(volume_curve_));

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

TEST_F(AudioOutputTest, ProcessRequestsSilenceIfNoSourceBuffer) {
  auto format = Format::Create({
                                   .sample_format = fuchsia::media::AudioSampleFormat::FLOAT,
                                   .channels = 2,
                                   .frames_per_second = 48000,
                               })
                    .take_value();
  // Use an output pipeline that will always return nullopt from ReadLock.
  auto pipeline_owned = std::make_unique<TestOutputPipeline>(format);
  audio_output_->set_output_pipeline(std::move(pipeline_owned));

  static const TimelineFunction kOneFramePerMs = TimelineFunction(TimelineRate(1, 1'000'000));
  static PipelineConfig config = PipelineConfig::Default();
  static VolumeCurve volume_curve =
      VolumeCurve::DefaultForMinGain(VolumeCurve::kDefaultGainForMinVolume);
  audio_output_->SetupMixTask(config, volume_curve, /* channels */ 2, zx::msec(1).to_msecs(),
                              kOneFramePerMs);

  // Return some valid, non-silent frame range from StartMixJob.
  audio_output_->set_start_mix_delegate([](zx::time now) {
    return TestAudioOutput::FrameSpan{
        .start = 0,
        .length = 100,
        .is_mute = false,
    };
  });

  bool finish_called = false;
  audio_output_->set_finish_mix_delegate([&finish_called](auto span, auto buffer) {
    EXPECT_EQ(span.start, 0);
    EXPECT_EQ(span.length, 100u);
    EXPECT_TRUE(span.is_mute);
    EXPECT_EQ(buffer, nullptr);
    finish_called = true;
  });

  // Now do a mix.
  audio_output_->Process();
  EXPECT_TRUE(finish_called);
}

// Verify we call StartMixJob multiple times if FinishMixJob does not fill buffer.
TEST_F(AudioOutputTest, ProcessMultipleMixJobs) {
  const Format format =
      Format::Create({
                         .sample_format = fuchsia::media::AudioSampleFormat::FLOAT,
                         .channels = 2,
                         .frames_per_second = 48000,
                     })
          .take_value();
  // Use an output pipeline that will always return nullopt from ReadLock.
  auto pipeline_owned = std::make_unique<TestOutputPipeline>(format);
  auto pipeline = pipeline_owned.get();
  audio_output_->set_output_pipeline(std::move(pipeline_owned));

  static const TimelineFunction kOneFramePerMs = TimelineFunction(TimelineRate(1, 1'000'000));
  static PipelineConfig config = PipelineConfig::Default();
  static VolumeCurve volume_curve =
      VolumeCurve::DefaultForMinGain(VolumeCurve::kDefaultGainForMinVolume);
  audio_output_->SetupMixTask(config, volume_curve, /* channels */ 2, zx::msec(1).to_msecs(),
                              kOneFramePerMs);

  const uint32_t kBufferFrames = 25;
  const uint32_t kBufferSamples = kBufferFrames * 2;
  const uint32_t kNumBuffers = 4;
  // Setup our buffer with data that is just the value of frame 'N' is 'N'.
  std::vector<float> buffer(kBufferSamples);
  for (size_t sample = 0; sample < kBufferSamples; ++sample) {
    buffer[sample] = static_cast<float>(sample);
  }
  // Enqueue several buffers, each with the same payload buffer.
  for (size_t i = 0; i < kNumBuffers; ++i) {
    pipeline->Enqueue(ReadableStream::Buffer(i * kBufferFrames, kBufferFrames, buffer.data(), true,
                                             StreamUsageMask(), Gain::kUnityGainDb));
  }

  // Return some valid, non-silent frame range from StartMixJob.
  uint32_t mix_jobs = 0;
  uint32_t frames_finished = 0;
  audio_output_->set_start_mix_delegate([&frames_finished, &mix_jobs](zx::time now) {
    ++mix_jobs;
    return TestAudioOutput::FrameSpan{
        .start = frames_finished,
        .length = (kBufferFrames * kNumBuffers) - frames_finished,
        .is_mute = false,
    };
  });

  audio_output_->set_finish_mix_delegate([&frames_finished](auto span, auto buffer) {
    EXPECT_EQ(span.start, frames_finished);
    EXPECT_FALSE(span.is_mute);
    EXPECT_NE(buffer, nullptr);
    for (size_t sample = 0; sample < kBufferSamples; ++sample) {
      EXPECT_FLOAT_EQ(static_cast<float>(sample), buffer[sample]);
    }
    frames_finished += span.length;
  });

  // Now do a mix.
  audio_output_->Process();
  EXPECT_EQ(frames_finished, kNumBuffers * kBufferFrames);
  EXPECT_EQ(mix_jobs, kNumBuffers);
}

}  // namespace
}  // namespace media::audio
