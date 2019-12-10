// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <fuchsia/media/cpp/fidl.h>
#include <fuchsia/media/playback/cpp/fidl.h>
#include <fuchsia/sys/cpp/fidl.h>
#include <fuchsia/ui/views/cpp/fidl.h>
#include <lib/sys/cpp/testing/test_with_environment.h>

#include <queue>

#include "gtest/gtest.h"
#include "lib/media/cpp/timeline_function.h"
#include "lib/media/cpp/type_converters.h"
#include "lib/ui/scenic/cpp/view_token_pair.h"
#include "lib/zx/time.h"
#include "src/lib/fsl/io/fd.h"
#include "src/lib/syslog/cpp/logger.h"
#include "src/media/playback/mediaplayer/test/command_queue.h"
#include "src/media/playback/mediaplayer/test/fakes/fake_audio.h"
#include "src/media/playback/mediaplayer/test/fakes/fake_scenic.h"
#include "src/media/playback/mediaplayer/test/fakes/fake_wav_reader.h"
#include "src/media/playback/mediaplayer/test/sink_feeder.h"

namespace media_player {
namespace test {

static constexpr uint16_t kSamplesPerFrame = 2;      // Stereo
static constexpr uint32_t kFramesPerSecond = 48000;  // 48kHz
static constexpr size_t kVmoSize = 1024;
static constexpr uint32_t kNumVmos = 4;

// Base class for audio consumer tests.
class AudioConsumerTests : public sys::testing::TestWithEnvironment {
 protected:
  void SetUp() override {
    syslog::InitLogger({"mediaplayer"});

    auto services = CreateServices();

    // Add the service under test using its launch info.
    fuchsia::sys::LaunchInfo launch_info{
        "fuchsia-pkg://fuchsia.com/mediaplayer#meta/mediaplayer.cmx"};
    zx_status_t status = services->AddServiceWithLaunchInfo(
        std::move(launch_info), fuchsia::media::SessionAudioConsumerFactory::Name_);
    EXPECT_EQ(ZX_OK, status);

    services->AddService(fake_audio_.GetRequestHandler());
    services->AllowParentService("fuchsia.logger.LogSink");

    // Create the synthetic environment.
    environment_ = CreateNewEnclosingEnvironment("mediaplayer_tests", std::move(services),
                                                 {.inherit_parent_services = true});

    fuchsia::media::SessionAudioConsumerFactoryPtr session_audio_consumer_factory;
    // Instantiate the audio consumer under test.
    environment_->ConnectToService(session_audio_consumer_factory.NewRequest());

    WaitForEnclosingEnvToStart(environment_.get());

    session_audio_consumer_factory.set_error_handler([](zx_status_t status) {
      FX_LOGS(WARNING) << "Audio consumer factory connection closed, status " << status << ".";
    });

    session_audio_consumer_factory->CreateAudioConsumer(0, audio_consumer_.NewRequest());

    audio_consumer_.set_error_handler([this](zx_status_t status) {
      FX_LOGS(ERROR) << "Audio consumer connection closed, status " << status << ".";
      audio_consumer_connection_closed_ = true;
      QuitLoop();
    });
  }

  void TearDown() override { EXPECT_FALSE(audio_consumer_connection_closed_); }

  fuchsia::media::AudioConsumerPtr audio_consumer_;
  bool audio_consumer_connection_closed_;
  bool got_status_;

  FakeAudio fake_audio_;
  std::unique_ptr<sys::testing::EnclosingEnvironment> environment_;
  SinkFeeder sink_feeder_;
};

// Test that factory channel is closed and we still have a connection to the created AudioConsumer
TEST_F(AudioConsumerTests, FactoryClosed) {
  audio_consumer_.events().WatchStatus(
      [this](fuchsia::media::AudioConsumerStatus status) { got_status_ = true; });

  RunLoopUntil([this]() { return got_status_; });

  EXPECT_FALSE(audio_consumer_connection_closed_);
}

// Test packet flow of AudioConsumer interface by using a synthetic environment
// to push a packet through and checking that it is processed.
TEST_F(AudioConsumerTests, CreateStreamSink) {
  fuchsia::media::StreamSinkPtr sink;
  fuchsia::media::AudioStreamType stream_type;
  stream_type.frames_per_second = kFramesPerSecond;
  stream_type.channels = kSamplesPerFrame;
  stream_type.sample_format = fuchsia::media::AudioSampleFormat::SIGNED_16;
  bool sink_connection_closed = false;

  got_status_ = false;

  auto compression = fuchsia::media::Compression::New();
  compression->type = fuchsia::media::AUDIO_ENCODING_AACLATM;

  std::vector<zx::vmo> vmos(kNumVmos);
  for (uint32_t i = 0; i < kNumVmos; i++) {
    zx_status_t status = zx::vmo::create(kVmoSize, 0, &vmos[i]);
    EXPECT_EQ(status, ZX_OK);
  }

  audio_consumer_->CreateStreamSink(std::move(vmos), stream_type, std::move(compression),
                                    sink.NewRequest());

  sink.set_error_handler(
      [&sink_connection_closed](zx_status_t status) { sink_connection_closed = true; });

  RunLoopUntilIdle();

  audio_consumer_->Start(fuchsia::media::AudioConsumerStartFlags::SUPPLY_DRIVEN, 0,
                         fuchsia::media::NO_TIMESTAMP);

  audio_consumer_.events().WatchStatus([this](fuchsia::media::AudioConsumerStatus status) {
    EXPECT_TRUE(status.has_presentation_timeline());
    // test things are progressing
    EXPECT_EQ(status.presentation_timeline().subject_delta, 1u);
    got_status_ = true;
  });

  RunLoopUntil([this]() { return got_status_; });

  auto packet = fuchsia::media::StreamPacket::New();
  packet->payload_buffer_id = 0;
  packet->payload_size = kVmoSize;
  packet->payload_offset = 0;
  packet->pts = fuchsia::media::NO_TIMESTAMP;

  bool sent_packet = false;
  sink->SendPacket(*packet, [&sent_packet]() { sent_packet = true; });

  RunLoopUntil([&sent_packet]() { return sent_packet; });

  EXPECT_TRUE(sent_packet);
  EXPECT_FALSE(sink_connection_closed);
}

// Test expected behavior of AudioConsumer interface when no compression type is
// set when creating a StreamSink
TEST_F(AudioConsumerTests, NoCompression) {
  fuchsia::media::StreamSinkPtr sink;
  fuchsia::media::AudioStreamType stream_type;
  stream_type.frames_per_second = kFramesPerSecond;
  stream_type.channels = kSamplesPerFrame;
  stream_type.sample_format = fuchsia::media::AudioSampleFormat::SIGNED_16;
  bool sink_connection_closed = false;
  got_status_ = false;

  std::vector<zx::vmo> vmos(kNumVmos);
  for (uint32_t i = 0; i < kNumVmos; i++) {
    zx_status_t status = zx::vmo::create(kVmoSize, 0, &vmos[i]);
    EXPECT_EQ(status, ZX_OK);
  }

  audio_consumer_->CreateStreamSink(std::move(vmos), stream_type, nullptr, sink.NewRequest());

  audio_consumer_->Start(fuchsia::media::AudioConsumerStartFlags::SUPPLY_DRIVEN, 0,
                         fuchsia::media::NO_TIMESTAMP);

  audio_consumer_.events().WatchStatus(
      [this](fuchsia::media::AudioConsumerStatus status) { got_status_ = true; });

  sink.set_error_handler(
      [&sink_connection_closed](zx_status_t status) { sink_connection_closed = true; });

  RunLoopUntil([this]() { return got_status_; });

  EXPECT_TRUE(got_status_);
  EXPECT_FALSE(sink_connection_closed);
}

// Test that creating multiple StreamSink's back to back results in both
// returned sinks functioning correctly
TEST_F(AudioConsumerTests, MultipleSinks) {
  fuchsia::media::AudioStreamType stream_type;
  stream_type.frames_per_second = kFramesPerSecond;
  stream_type.channels = kSamplesPerFrame;
  stream_type.sample_format = fuchsia::media::AudioSampleFormat::SIGNED_16;

  {
    got_status_ = false;
    fuchsia::media::StreamSinkPtr sink;
    std::vector<zx::vmo> vmos(kNumVmos);

    for (uint32_t i = 0; i < kNumVmos; i++) {
      zx_status_t status = zx::vmo::create(kVmoSize, 0, &vmos[i]);
      EXPECT_EQ(status, ZX_OK);
    }

    auto compression = fuchsia::media::Compression::New();
    compression->type = fuchsia::media::AUDIO_ENCODING_LPCM;

    audio_consumer_->CreateStreamSink(std::move(vmos), stream_type, std::move(compression),
                                      sink.NewRequest());

    audio_consumer_->Start(fuchsia::media::AudioConsumerStartFlags::SUPPLY_DRIVEN, 0,
                           fuchsia::media::NO_TIMESTAMP);

    audio_consumer_.events().WatchStatus([this](fuchsia::media::AudioConsumerStatus status) {
      EXPECT_TRUE(status.has_presentation_timeline());
      // test things are progressing
      EXPECT_EQ(status.presentation_timeline().subject_delta, 1u);
      got_status_ = true;
    });

    RunLoopUntil([this]() { return got_status_; });

    EXPECT_TRUE(got_status_);

    got_status_ = false;
  }

  audio_consumer_->Stop();

  {
    fuchsia::media::StreamSinkPtr sink;

    std::vector<zx::vmo> vmos(kNumVmos);
    for (uint32_t i = 0; i < kNumVmos; i++) {
      zx_status_t status = zx::vmo::create(kVmoSize, 0, &vmos[i]);
      EXPECT_EQ(status, ZX_OK);
    }

    auto compression = fuchsia::media::Compression::New();
    compression->type = fuchsia::media::AUDIO_ENCODING_LPCM;

    audio_consumer_->CreateStreamSink(std::move(vmos), stream_type, std::move(compression),
                                      sink.NewRequest());

    audio_consumer_->Start(fuchsia::media::AudioConsumerStartFlags::SUPPLY_DRIVEN, 0,
                           fuchsia::media::NO_TIMESTAMP);

    audio_consumer_.events().WatchStatus([this](fuchsia::media::AudioConsumerStatus status) {
      EXPECT_TRUE(status.has_presentation_timeline());
      // test things are progressing
      EXPECT_EQ(status.presentation_timeline().subject_delta, 1u);
      got_status_ = true;
    });

    RunLoopUntil([this]() { return got_status_; });

    EXPECT_TRUE(got_status_);
  }
}

// Test that multiple stream sinks can be created at the same time, but packets
// can only be sent on the most recently active one. Also test that packets can
// be queued on the 'pending' sink.
TEST_F(AudioConsumerTests, OverlappingStreamSink) {
  fuchsia::media::StreamSinkPtr sink2;
  bool sink2_packet = false;
  got_status_ = false;

  fuchsia::media::AudioStreamType stream_type;
  stream_type.frames_per_second = kFramesPerSecond;
  stream_type.channels = kSamplesPerFrame;
  stream_type.sample_format = fuchsia::media::AudioSampleFormat::SIGNED_16;

  auto packet = fuchsia::media::StreamPacket::New();
  packet->payload_buffer_id = 0;
  packet->payload_size = kVmoSize;
  packet->payload_offset = 0;
  packet->pts = fuchsia::media::NO_TIMESTAMP;

  {
    fuchsia::media::StreamSinkPtr sink1;

    auto compression1 = fuchsia::media::Compression::New();
    compression1->type = fuchsia::media::AUDIO_ENCODING_LPCM;

    auto compression2 = fuchsia::media::Compression::New();
    compression2->type = fuchsia::media::AUDIO_ENCODING_LPCM;

    std::vector<zx::vmo> vmos1(kNumVmos);
    for (uint32_t i = 0; i < kNumVmos; i++) {
      zx_status_t status = zx::vmo::create(kVmoSize, 0, &vmos1[i]);
      EXPECT_EQ(status, ZX_OK);
    }

    std::vector<zx::vmo> vmos2(kNumVmos);
    for (uint32_t i = 0; i < kNumVmos; i++) {
      zx_status_t status = zx::vmo::create(kVmoSize, 0, &vmos2[i]);
      EXPECT_EQ(status, ZX_OK);
    }

    audio_consumer_->CreateStreamSink(std::move(vmos1), stream_type, std::move(compression1),
                                      sink1.NewRequest());

    audio_consumer_->CreateStreamSink(std::move(vmos2), stream_type, std::move(compression2),
                                      sink2.NewRequest());

    audio_consumer_->Start(fuchsia::media::AudioConsumerStartFlags::SUPPLY_DRIVEN, 0,
                           fuchsia::media::NO_TIMESTAMP);

    audio_consumer_.events().WatchStatus([this](fuchsia::media::AudioConsumerStatus status) {
      EXPECT_TRUE(status.has_presentation_timeline());
      // test things are progressing
      EXPECT_EQ(status.presentation_timeline().subject_delta, 1u);
      got_status_ = true;
    });

    bool sink1_packet = false;
    sink1->SendPacket(*packet, [&sink1_packet]() { sink1_packet = true; });

    RunLoopUntil([&sink1_packet]() { return sink1_packet; });

    EXPECT_TRUE(sink1_packet);
    EXPECT_FALSE(sink2_packet);
  }

  // sink 1 dropped, now should be getting packets flowing from sink2
  sink2->SendPacket(*packet, [&sink2_packet]() { sink2_packet = true; });

  RunLoopUntil([&sink2_packet]() { return sink2_packet; });

  EXPECT_TRUE(sink2_packet);
}

}  // namespace test
}  // namespace media_player
