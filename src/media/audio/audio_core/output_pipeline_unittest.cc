// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/audio_core/output_pipeline.h"

#include <gmock/gmock.h>

#include "src/media/audio/audio_core/packet_queue.h"
#include "src/media/audio/audio_core/process_config.h"
#include "src/media/audio/audio_core/testing/packet_factory.h"
#include "src/media/audio/audio_core/testing/threading_model_fixture.h"
#include "src/media/audio/audio_core/usage_settings.h"
#include "src/media/audio/lib/effects_loader/testing/test_effects.h"

using testing::Each;
using testing::Eq;
using testing::Pointwise;

namespace media::audio {
namespace {

const Format kDefaultFormat =
    Format::Create(fuchsia::media::AudioStreamType{
                       .sample_format = fuchsia::media::AudioSampleFormat::FLOAT,
                       .channels = 2,
                       .frames_per_second = 48000,
                   })
        .take_value();

const TimelineFunction kDefaultTransform = TimelineFunction(
    TimelineRate(FractionalFrames<uint32_t>(kDefaultFormat.frames_per_second()).raw_value(),
                 zx::sec(1).to_nsecs()));

class OutputPipelineTest : public testing::ThreadingModelFixture {
 protected:
  std::shared_ptr<OutputPipeline> CreateOutputPipeline() {
    ProcessConfig::Builder builder;
    PipelineConfig::MixGroup root{
        .name = "linearize",
        .input_streams =
            {
                RenderUsage::BACKGROUND,
            },
        .effects = {},
        .inputs = {{
            .name = "mix",
            .input_streams =
                {
                    RenderUsage::INTERRUPTION,
                },
            .effects = {},
            .inputs = {{
                           .name = "default",
                           .input_streams =
                               {
                                   RenderUsage::MEDIA,
                                   RenderUsage::SYSTEM_AGENT,
                               },
                           .effects = {},
                           .loopback = false,
                           .output_rate = 48000,
                       },
                       {
                           .name = "communications",
                           .input_streams =
                               {
                                   RenderUsage::COMMUNICATION,
                               },
                           .effects = {},
                           .loopback = false,
                           .output_rate = 48000,
                       }},
            .loopback = false,
            .output_rate = 48000,

        }},
        .loopback = false,
        .output_rate = 48000,
    };

    auto pipeline_config = PipelineConfig(root);
    return std::make_shared<OutputPipeline>(pipeline_config, kDefaultFormat.channels(), 128,
                                            kDefaultTransform);
  }

  void CheckBuffer(void* buffer, float expected_sample, size_t num_samples) {
    float* floats = reinterpret_cast<float*>(buffer);
    for (size_t i = 0; i < num_samples; ++i) {
      ASSERT_FLOAT_EQ(expected_sample, floats[i]);
    }
  }
};

TEST_F(OutputPipelineTest, Trim) {
  auto timeline_function = fbl::MakeRefCounted<VersionedTimelineFunction>(kDefaultTransform);
  auto stream1 = std::make_shared<PacketQueue>(kDefaultFormat, timeline_function);
  auto stream2 = std::make_shared<PacketQueue>(kDefaultFormat, timeline_function);
  auto stream3 = std::make_shared<PacketQueue>(kDefaultFormat, timeline_function);
  auto stream4 = std::make_shared<PacketQueue>(kDefaultFormat, timeline_function);

  // Add some streams so that one is routed to each mix stage in our pipeline.
  auto pipeline = CreateOutputPipeline();
  pipeline->AddInput(stream1, StreamUsage::WithRenderUsage(RenderUsage::BACKGROUND));
  pipeline->AddInput(stream2, StreamUsage::WithRenderUsage(RenderUsage::INTERRUPTION));
  pipeline->AddInput(stream3, StreamUsage::WithRenderUsage(RenderUsage::MEDIA));
  pipeline->AddInput(stream4, StreamUsage::WithRenderUsage(RenderUsage::COMMUNICATION));

  bool packet_released[8] = {};
  testing::PacketFactory packet_factory1(dispatcher(), kDefaultFormat, PAGE_SIZE);
  {
    stream1->PushPacket(packet_factory1.CreatePacket(
        1.0, zx::msec(5), [&packet_released] { packet_released[0] = true; }));
    stream1->PushPacket(packet_factory1.CreatePacket(
        1.0, zx::msec(5), [&packet_released] { packet_released[1] = true; }));
  }
  testing::PacketFactory packet_factory2(dispatcher(), kDefaultFormat, PAGE_SIZE);
  {
    stream2->PushPacket(packet_factory2.CreatePacket(
        1.0, zx::msec(5), [&packet_released] { packet_released[2] = true; }));
    stream2->PushPacket(packet_factory2.CreatePacket(
        1.0, zx::msec(5), [&packet_released] { packet_released[3] = true; }));
  }
  testing::PacketFactory packet_factory3(dispatcher(), kDefaultFormat, PAGE_SIZE);
  {
    stream3->PushPacket(packet_factory3.CreatePacket(
        1.0, zx::msec(5), [&packet_released] { packet_released[4] = true; }));
    stream3->PushPacket(packet_factory3.CreatePacket(
        1.0, zx::msec(5), [&packet_released] { packet_released[5] = true; }));
  }
  testing::PacketFactory packet_factory4(dispatcher(), kDefaultFormat, PAGE_SIZE);
  {
    stream4->PushPacket(packet_factory4.CreatePacket(
        1.0, zx::msec(5), [&packet_released] { packet_released[6] = true; }));
    stream4->PushPacket(packet_factory4.CreatePacket(
        1.0, zx::msec(5), [&packet_released] { packet_released[7] = true; }));
  }

  // After 4ms we should still be retaining all packets.
  pipeline->Trim(zx::time(0) + zx::msec(4));
  RunLoopUntilIdle();
  EXPECT_THAT(packet_released, Each(Eq(false)));

  // At 5ms we should have trimmed the first packet from each queue.
  pipeline->Trim(zx::time(0) + zx::msec(5));
  RunLoopUntilIdle();
  EXPECT_THAT(packet_released,
              Pointwise(Eq(), {true, false, true, false, true, false, true, false}));

  // After 10ms we should have trimmed all the packets.
  pipeline->Trim(zx::time(0) + zx::msec(10));
  RunLoopUntilIdle();
  EXPECT_THAT(packet_released, Each(Eq(true)));
}

TEST_F(OutputPipelineTest, Loopback) {
  auto test_effects = testing::TestEffectsModule::Open();
  test_effects.AddEffect("add_1.0").WithAction(TEST_EFFECTS_ACTION_ADD, 1.0);
  PipelineConfig::MixGroup root{
      .name = "linearize",
      .input_streams =
          {
              RenderUsage::BACKGROUND,
          },
      .effects =
          {
              {
                  .lib_name = "test_effects.so",
                  .effect_name = "add_1.0",
                  .instance_name = "",
                  .effect_config = "",
              },
          },
      .inputs = {{
          .name = "mix",
          .input_streams =
              {
                  RenderUsage::MEDIA,
                  RenderUsage::SYSTEM_AGENT,
                  RenderUsage::INTERRUPTION,
                  RenderUsage::COMMUNICATION,
              },
          .effects =
              {
                  {
                      .lib_name = "test_effects.so",
                      .effect_name = "add_1.0",
                      .instance_name = "",
                      .effect_config = "",
                  },
              },
          .loopback = true,
          .output_rate = 48000,
      }},
      .loopback = false,
      .output_rate = 48000,
  };
  auto pipeline_config = PipelineConfig(root);
  auto pipeline = std::make_shared<OutputPipeline>(pipeline_config, kDefaultFormat.channels(), 128,
                                                   kDefaultTransform);

  // Verify our stream from the pipeline has the effects applied (we have no input streams so we
  // should have silence with a two effects that adds 1.0 to each sample (one on the mix stage
  // and one on the linearize stage). Therefore we expect all samples to be 2.0.
  auto buf = pipeline->LockBuffer(zx::time(0), 0, 48);
  ASSERT_TRUE(buf);
  ASSERT_EQ(buf->start().Floor(), 0u);
  ASSERT_EQ(buf->length().Floor(), 48u);
  CheckBuffer(buf->payload(), 2.0, 96);

  // We loopback after the mix stage and before the linearize stage. So we should observe only a
  // single effects pass. Therefore we expect all loopback samples to be 1.0.
  auto transform = pipeline->loopback()->ReferenceClockToFractionalFrames();
  auto loopback_frame =
      FractionalFrames<int64_t>::FromRaw(transform.timeline_function.Apply((zx::time(0)).get()))
          .Floor();
  auto loopback_buf =
      pipeline->loopback()->LockBuffer(zx::time(0) + zx::msec(1), loopback_frame, 48);
  ASSERT_TRUE(loopback_buf);
  ASSERT_EQ(loopback_buf->start().Floor(), 0u);
  ASSERT_EQ(loopback_buf->length().Floor(), 48u);
  CheckBuffer(loopback_buf->payload(), 1.0, 96);
}

// Identical to |Loopback|, except we run mix and linearize stages at different rates.
TEST_F(OutputPipelineTest, LoopbackWithUpsample) {
  auto test_effects = testing::TestEffectsModule::Open();
  test_effects.AddEffect("add_1.0").WithAction(TEST_EFFECTS_ACTION_ADD, 1.0);
  PipelineConfig::MixGroup root{
      .name = "linearize",
      .input_streams =
          {
              RenderUsage::BACKGROUND,
          },
      .effects =
          {
              {
                  .lib_name = "test_effects.so",
                  .effect_name = "add_1.0",
                  .instance_name = "",
                  .effect_config = "",
              },
          },
      .inputs = {{
          .name = "mix",
          .input_streams =
              {
                  RenderUsage::MEDIA,
                  RenderUsage::SYSTEM_AGENT,
                  RenderUsage::INTERRUPTION,
                  RenderUsage::COMMUNICATION,
              },
          .effects =
              {
                  {
                      .lib_name = "test_effects.so",
                      .effect_name = "add_1.0",
                      .instance_name = "",
                      .effect_config = "",
                  },
              },
          .loopback = true,
          .output_rate = 48000,
      }},
      .loopback = false,
      .output_rate = 96000,
  };
  auto pipeline_config = PipelineConfig(root);
  auto pipeline = std::make_shared<OutputPipeline>(pipeline_config, kDefaultFormat.channels(), 128,
                                                   kDefaultTransform);

  // Verify our stream from the pipeline has the effects applied (we have no input streams so we
  // should have silence with a two effects that adds 1.0 to each sample (one on the mix stage
  // and one on the linearize stage). Therefore we expect all samples to be 2.0.
  auto buf = pipeline->LockBuffer(zx::time(0), 0, 96);
  ASSERT_TRUE(buf);
  ASSERT_EQ(buf->start().Floor(), 0u);
  ASSERT_EQ(buf->length().Floor(), 96u);
  CheckBuffer(buf->payload(), 2.0, 192);

  // We loopback after the mix stage and before the linearize stage. So we should observe only a
  // single effects pass. Therefore we expect all loopback samples to be 1.0.
  auto transform = pipeline->loopback()->ReferenceClockToFractionalFrames();
  auto loopback_frame =
      FractionalFrames<int64_t>::FromRaw(transform.timeline_function.Apply((zx::time(0)).get()))
          .Floor();
  auto loopback_buf =
      pipeline->loopback()->LockBuffer(zx::time(0) + zx::msec(1), loopback_frame, 48);
  ASSERT_TRUE(loopback_buf);
  ASSERT_EQ(loopback_buf->start().Floor(), 0u);
  ASSERT_EQ(loopback_buf->length().Floor(), 48u);
  CheckBuffer(loopback_buf->payload(), 1.0, 96);
}

static const std::string kInstanceName = "instance name";
static const std::string kConfig = "config";

TEST_F(OutputPipelineTest, SetEffectConfig) {
  auto test_effects = testing::TestEffectsModule::Open();
  test_effects.AddEffect("assign_config_size")
      .WithAction(TEST_EFFECTS_ACTION_ASSIGN_CONFIG_SIZE, 0.0);
  PipelineConfig::MixGroup root{
      .name = "linearize",
      .input_streams =
          {
              RenderUsage::BACKGROUND,
          },
      .effects =
          {
              {
                  .lib_name = "test_effects.so",
                  .effect_name = "assign_config_size",
                  .instance_name = kInstanceName,
                  .effect_config = "",
              },
          },
      .inputs = {{
          .name = "mix",
          .input_streams =
              {
                  RenderUsage::MEDIA,
                  RenderUsage::SYSTEM_AGENT,
                  RenderUsage::INTERRUPTION,
                  RenderUsage::COMMUNICATION,
              },
          .effects = {},
          .output_rate = 48000,
      }},
      .output_rate = 48000,
  };
  auto pipeline_config = PipelineConfig(root);
  auto pipeline = std::make_shared<OutputPipeline>(pipeline_config, kDefaultFormat.channels(), 128,
                                                   kDefaultTransform);

  pipeline->SetEffectConfig(kInstanceName, kConfig);

  // Verify our stream from the pipeline has the effects applied (we have no input streams so we
  // should have silence with a single effect that sets all samples to the size of the new config).
  auto buf = pipeline->LockBuffer(zx::time(0) + zx::msec(1), 0, 48);
  ASSERT_TRUE(buf);
  ASSERT_EQ(buf->start().Floor(), 0u);
  ASSERT_EQ(buf->length().Floor(), 48u);
  float expected_sample = static_cast<float>(kConfig.size());
  CheckBuffer(buf->payload(), expected_sample, 96);
}

TEST_F(OutputPipelineTest, DifferentMixRates) {
  static const PipelineConfig::MixGroup root{
      .name = "linearize",
      .input_streams =
          {
              RenderUsage::BACKGROUND,
          },
      .inputs = {{
          .name = "mix",
          .input_streams =
              {
                  RenderUsage::MEDIA,
                  RenderUsage::SYSTEM_AGENT,
                  RenderUsage::INTERRUPTION,
                  RenderUsage::COMMUNICATION,
              },
          .effects = {},
          .loopback = true,
          .output_rate = 24000,
      }},
      .loopback = false,
      .output_rate = 48000,
  };
  testing::PacketFactory packet_factory1(dispatcher(), kDefaultFormat, PAGE_SIZE);
  // Add the stream with a usage that routes to the mix stage. We request a simple point sampler
  // to make data verficications a bit simpler.
  const Mixer::Resampler resampler = Mixer::Resampler::SampleAndHold;
  auto timeline_function = fbl::MakeRefCounted<VersionedTimelineFunction>(kDefaultTransform);
  auto stream1 = std::make_shared<PacketQueue>(kDefaultFormat, timeline_function);
  auto pipeline_config = PipelineConfig(root);
  auto pipeline = std::make_shared<OutputPipeline>(pipeline_config, kDefaultFormat.channels(), 480,
                                                   kDefaultTransform, resampler);

  pipeline->AddInput(stream1, StreamUsage::WithRenderUsage(RenderUsage::MEDIA), resampler);

  bool packet_released[2] = {};
  {
    stream1->PushPacket(packet_factory1.CreatePacket(
        1.0, zx::msec(5), [&packet_released] { packet_released[0] = true; }));
    stream1->PushPacket(packet_factory1.CreatePacket(
        100.0, zx::msec(5), [&packet_released] { packet_released[1] = true; }));
  }

  {
    auto buf = pipeline->LockBuffer(zx::time(0), 0, 240);
    RunLoopUntilIdle();

    EXPECT_TRUE(buf);
    EXPECT_TRUE(packet_released[0]);
    EXPECT_FALSE(packet_released[1]);
    EXPECT_EQ(buf->start().Floor(), 0u);
    EXPECT_EQ(buf->length().Floor(), 240u);
    CheckBuffer(buf->payload(), 1.0, 240);
  }

  {
    auto buf = pipeline->LockBuffer(zx::time(0) + zx::msec(10), 240, 240);
    RunLoopUntilIdle();

    EXPECT_TRUE(buf);
    EXPECT_TRUE(packet_released[0]);
    EXPECT_TRUE(packet_released[1]);
    EXPECT_EQ(buf->start().Floor(), 240u);
    EXPECT_EQ(buf->length().Floor(), 240u);
    CheckBuffer(buf->payload(), 100.0, 240);
  }
}

}  // namespace
}  // namespace media::audio
