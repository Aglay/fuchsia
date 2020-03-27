// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/audio_core/stream_volume_manager.h"

#include <lib/gtest/test_loop_fixture.h>

#include <gtest/gtest.h>

#include "src/media/audio/audio_core/mixer/gain.h"

namespace media::audio {
namespace {

using namespace testing;

class MockStreamVolume : public StreamVolume {
 public:
  bool GetStreamMute() const override { return mute_; }
  fuchsia::media::Usage GetStreamUsage() const override { return fidl::Clone(usage_); }
  bool RespectsPolicyAdjustments() const override { return respects_policy_adjustments_; }
  void RealizeVolume(VolumeCommand volume_command) override { volume_command_ = volume_command; }

  bool mute_ = false;
  fuchsia::media::Usage usage_;
  VolumeCommand volume_command_ = {};
  bool respects_policy_adjustments_ = true;
};

class StreamVolumeManagerTest : public ::gtest::TestLoopFixture {
 protected:
  StreamVolumeManagerTest() : manager_(dispatcher()) {}

  fuchsia::media::audio::VolumeControlPtr AddClientForUsage(fuchsia::media::Usage usage) {
    fuchsia::media::audio::VolumeControlPtr volume_control_ptr;
    manager_.BindUsageVolumeClient(std::move(usage), volume_control_ptr.NewRequest(dispatcher()));
    return volume_control_ptr;
  }

  MockStreamVolume mock_;
  StreamVolumeManager manager_;
};

TEST_F(StreamVolumeManagerTest, StreamCanUpdateSelf) {
  mock_.usage_ =
      fuchsia::media::Usage::WithRenderUsage(fuchsia::media::AudioRenderUsage::INTERRUPTION);

  manager_.NotifyStreamChanged(&mock_);
  EXPECT_FLOAT_EQ(mock_.volume_command_.volume, 1.0);
  EXPECT_FLOAT_EQ(mock_.volume_command_.gain_db_adjustment, Gain::kUnityGainDb);
  EXPECT_EQ(mock_.volume_command_.ramp, std::nullopt);
}

TEST_F(StreamVolumeManagerTest, StreamUpdatedOnAdd) {
  mock_.usage_ =
      fuchsia::media::Usage::WithRenderUsage(fuchsia::media::AudioRenderUsage::INTERRUPTION);

  manager_.AddStream(&mock_);
  EXPECT_FLOAT_EQ(mock_.volume_command_.volume, 1.0);
  EXPECT_FLOAT_EQ(mock_.volume_command_.gain_db_adjustment, Gain::kUnityGainDb);
  EXPECT_EQ(mock_.volume_command_.ramp, std::nullopt);
}

TEST_F(StreamVolumeManagerTest, StreamCanIgnorePolicy) {
  const auto usage =
      fuchsia::media::Usage::WithRenderUsage(fuchsia::media::AudioRenderUsage::INTERRUPTION);
  mock_.usage_ = fidl::Clone(usage);

  manager_.SetUsageGainAdjustment(fidl::Clone(usage), Gain::kMinGainDb);

  manager_.NotifyStreamChanged(&mock_);
  EXPECT_FLOAT_EQ(mock_.volume_command_.gain_db_adjustment, Gain::kMinGainDb);

  mock_.respects_policy_adjustments_ = false;
  manager_.NotifyStreamChanged(&mock_);
  EXPECT_FLOAT_EQ(mock_.volume_command_.gain_db_adjustment, 0.0);
}

TEST_F(StreamVolumeManagerTest, UsageChangesUpdateRegisteredStreams) {
  mock_.usage_ =
      fuchsia::media::Usage::WithRenderUsage(fuchsia::media::AudioRenderUsage::SYSTEM_AGENT);

  manager_.AddStream(&mock_);
  manager_.SetUsageGain(
      fuchsia::media::Usage::WithRenderUsage(fuchsia::media::AudioRenderUsage::SYSTEM_AGENT),
      -10.0);

  EXPECT_FLOAT_EQ(mock_.volume_command_.gain_db_adjustment, -10.0);
}

TEST_F(StreamVolumeManagerTest, StreamMuteIsConsidered) {
  mock_.mute_ = true;
  mock_.usage_ =
      fuchsia::media::Usage::WithRenderUsage(fuchsia::media::AudioRenderUsage::SYSTEM_AGENT);

  manager_.AddStream(&mock_);
  manager_.SetUsageGain(
      fuchsia::media::Usage::WithRenderUsage(fuchsia::media::AudioRenderUsage::SYSTEM_AGENT), 0.0);

  EXPECT_EQ(mock_.volume_command_.gain_db_adjustment, fuchsia::media::audio::MUTED_GAIN_DB);
}

TEST_F(StreamVolumeManagerTest, StreamsCanBeRemoved) {
  mock_.usage_ =
      fuchsia::media::Usage::WithRenderUsage(fuchsia::media::AudioRenderUsage::SYSTEM_AGENT);

  manager_.AddStream(&mock_);
  manager_.RemoveStream(&mock_);
  manager_.SetUsageGain(
      fuchsia::media::Usage::WithRenderUsage(fuchsia::media::AudioRenderUsage::SYSTEM_AGENT), 10.0);

  EXPECT_FLOAT_EQ(mock_.volume_command_.volume, 1.0);
  EXPECT_FLOAT_EQ(mock_.volume_command_.gain_db_adjustment, Gain::kUnityGainDb);
  EXPECT_EQ(mock_.volume_command_.ramp, std::nullopt);
}

TEST_F(StreamVolumeManagerTest, StreamsCanRamp) {
  mock_.usage_ =
      fuchsia::media::Usage::WithRenderUsage(fuchsia::media::AudioRenderUsage::INTERRUPTION);

  manager_.NotifyStreamChanged(&mock_,
                               Ramp{zx::nsec(100), fuchsia::media::audio::RampType::SCALE_LINEAR});

  EXPECT_EQ(mock_.volume_command_.ramp->duration, zx::nsec(100));
  EXPECT_EQ(mock_.volume_command_.ramp->ramp_type, fuchsia::media::audio::RampType::SCALE_LINEAR);
}

TEST_F(StreamVolumeManagerTest, UsageVolumeChangeUpdatesStream) {
  MockStreamVolume media_stream;
  media_stream.usage_ =
      fuchsia::media::Usage::WithRenderUsage(fuchsia::media::AudioRenderUsage::MEDIA);

  MockStreamVolume system_agent_stream;
  system_agent_stream.usage_ =
      fuchsia::media::Usage::WithCaptureUsage(fuchsia::media::AudioCaptureUsage::SYSTEM_AGENT);

  manager_.AddStream(&media_stream);
  manager_.AddStream(&system_agent_stream);

  auto media_client = AddClientForUsage(
      fuchsia::media::Usage::WithRenderUsage(fuchsia::media::AudioRenderUsage::MEDIA));
  media_client->SetVolume(0.8);
  RunLoopUntilIdle();

  EXPECT_FLOAT_EQ(media_stream.volume_command_.volume, 0.8);
  EXPECT_FLOAT_EQ(system_agent_stream.volume_command_.volume, 1.0);

  auto system_client = AddClientForUsage(
      fuchsia::media::Usage::WithCaptureUsage(fuchsia::media::AudioCaptureUsage::SYSTEM_AGENT));
  system_client->SetVolume(0.9);
  RunLoopUntilIdle();
  EXPECT_FLOAT_EQ(media_stream.volume_command_.volume, 0.8);
  EXPECT_FLOAT_EQ(system_agent_stream.volume_command_.volume, 0.9);
}

}  // namespace
}  // namespace media::audio
