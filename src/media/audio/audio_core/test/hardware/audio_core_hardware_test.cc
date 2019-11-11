// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

#include "src/media/audio/audio_core/test/hardware/audio_core_hardware_test.h"

#include <lib/sys/cpp/service_directory.h>

#include "src/lib/syslog/cpp/logger.h"

namespace media::audio::test {

void AudioCoreHardwareTest::SetUp() {
  TestFixture::SetUp();

  ConnectToAudioCore();
  ConnectToAudioCapturer();

  GetDefaultCaptureFormat();
  SetCapturerFormat();

  MapMemoryForCapturer();

  RunLoopUntilIdle();
  ASSERT_FALSE(error_occurred());
}

void AudioCoreHardwareTest::TearDown() { ASSERT_FALSE(error_occurred()); }

void AudioCoreHardwareTest::ConnectToAudioCore() {
  audio_core_ = sys::ServiceDirectory::CreateFromNamespace()->Connect<fuchsia::media::AudioCore>();

  audio_core_.set_error_handler(ErrorHandler([](zx_status_t status) {
    FX_PLOGS(ERROR, status) << "Client connection to fuchsia.media.AudioCore failed";
  }));
}

void AudioCoreHardwareTest::ConnectToAudioCapturer() {
  ASSERT_TRUE(audio_core_.is_bound());

  constexpr bool kNotLoopback = false;
  audio_core_->CreateAudioCapturer(kNotLoopback, audio_capturer_.NewRequest());

  audio_capturer_.set_error_handler(ErrorHandler([](zx_status_t status) {
    FX_PLOGS(ERROR, status) << "Client connection to fuchsia.media.AudioCapturer failed";
  }));
}

// Fetch the initial media type and adjust channel_count_ and frames_per_second_ if needed.
void AudioCoreHardwareTest::GetDefaultCaptureFormat() {
  audio_capturer_->GetStreamType(CompletionCallback([this](fuchsia::media::StreamType stream_type) {
    ASSERT_TRUE(stream_type.medium_specific.is_audio()) << "Default format is not audio!";
    const auto& format = stream_type.medium_specific.audio();

    channel_count_ = format.channels;
    frames_per_second_ = format.frames_per_second;
  }));

  ExpectCallback();

  vmo_buffer_frame_count_ = (kBufferDurationMsec * frames_per_second_) / 1000;
  vmo_buffer_byte_count_ = vmo_buffer_frame_count_ * channel_count_ * kBytesPerSample;
}

// Capture in the input's default format, to minimize rate-conversion or rechannelization effects.
void AudioCoreHardwareTest::SetCapturerFormat() {
  fuchsia::media::AudioStreamType audio_stream_type;
  audio_stream_type.sample_format = kSampleFormat;
  audio_stream_type.channels = channel_count_;
  audio_stream_type.frames_per_second = frames_per_second_;

  audio_capturer_->SetPcmStreamType(audio_stream_type);
}

// Create a shared payload buffer, map it into our process, duplicate the VMO handle and pass it to
// the capturer as a payload buffer.
void AudioCoreHardwareTest::MapMemoryForCapturer() {
  zx::vmo audio_capturer_vmo;
  constexpr zx_vm_option_t kMapOptions = ZX_VM_PERM_READ;
  constexpr zx_rights_t kVmoRights =
      ZX_RIGHT_READ | ZX_RIGHT_WRITE | ZX_RIGHT_MAP | ZX_RIGHT_TRANSFER;

  zx_status_t status = payload_buffer_map_.CreateAndMap(vmo_buffer_byte_count_, kMapOptions,
                                                        /* vmar_manager= */ nullptr,
                                                        &audio_capturer_vmo, kVmoRights);
  EXPECT_EQ(status, ZX_OK) << "VmoMapper::CreateAndMap failed: " << status;

  audio_capturer_->AddPayloadBuffer(kPayloadBufferId, std::move(audio_capturer_vmo));

  payload_buffer_ = reinterpret_cast<float*>(payload_buffer_map_.start());
}

// A packet containing captured audio data was just returned to us -- handle it.
void AudioCoreHardwareTest::OnPacketProduced(fuchsia::media::StreamPacket pkt) {
  received_payload_frames_ = pkt.payload_size / (channel_count_ * kBytesPerSample);

  EXPECT_EQ(pkt.payload_offset, 0u);
  EXPECT_EQ(pkt.payload_size, vmo_buffer_byte_count_);
}

// Used when debugging repeatable test failures
void AudioCoreHardwareTest::DisplayReceivedAudio() {
  ASSERT_NE(payload_buffer_, nullptr);

  for (auto idx = 0u; idx < received_payload_frames_ * channel_count_; ++idx) {
    if (idx % 16 == 0) {
      printf("\n[%3x]", idx);
    }
    printf(" %8.05f", payload_buffer_[idx]);
  }
  printf("\n");
}

// When capturing from the real built-in microphone, the analog noise floor ensures that there
// should be at least 1 bit of ongoing broad-spectrum signal (excluding professional-grade
// products). Thus, if we are accurately capturing the analog noise floor, a long stretch of
// received 0.0 values should be uncommon. However, if our timing calculations are incorrect, then
// there could be sections of the capture buffer audio that were not written, and thus would present
// to us as a consecutive stretch of audio samples with value '0.0'.
//
// In short: to validate our capture-side mix pipeline timing, we will record an audio buffer from
// the live input device, then ensure that the longest stretch of consecutive '0.0' values received
// does not exceed a defined threshold.
//
// Note that we do this at the audio input device's native (default) frame_rate and channel_count,
// to minimize any loss in transparency from frame-rate-conversion or rechannelization.
TEST_F(AudioCoreHardwareTest, ZeroesInLiveCapture) {
  ASSERT_TRUE(audio_capturer_.is_bound());
  const uint32_t payload_offset = 0u;

  audio_capturer_->CaptureAt(kPayloadBufferId, payload_offset, vmo_buffer_frame_count_,
                             CompletionCallback([this](fuchsia::media::StreamPacket packet) {
                               OnPacketProduced(packet);
                             }));
  // Wait for the capture buffer to be returned.
  ExpectCallback();

  uint32_t count_consec_zero_samples = 0;
  uint32_t longest_stretch_consec_zero_samples = 0;

  ASSERT_NE(payload_buffer_, nullptr);
  for (auto idx = 0u; idx < received_payload_frames_ * channel_count_; ++idx) {
    if (payload_buffer_[idx] == 0.0f) {
      ++count_consec_zero_samples;
      if (count_consec_zero_samples > longest_stretch_consec_zero_samples) {
        longest_stretch_consec_zero_samples = count_consec_zero_samples;
      }
    } else {
      // Even if consecutive '0' samples is only a fraction of our limit, print to expose cadences.
      // In one failure mode we saw, 2-3 frames were consistently 0.0 at 50-ms boundaries.
      if (count_consec_zero_samples > (kLimitConsecFramesZero * channel_count_ / 2)) {
        printf("%d '0' samples ending at idx:%d\n", count_consec_zero_samples, idx);
      }
      count_consec_zero_samples = 0;
    }
  }

  if (longest_stretch_consec_zero_samples > kLimitConsecFramesZero * channel_count_) {
    if (longest_stretch_consec_zero_samples == received_payload_frames_ * channel_count_) {
      printf(
          "*** EVERY captured sample was '0'. Digital input, or not a real device, or capture gain "
          "muted the signal? ***\n");
    }

    EXPECT_LE(longest_stretch_consec_zero_samples, kLimitConsecFramesZero * channel_count_);
  } else {
    printf("Longest stretch of consecutive '0' samples was length %d (limit %d)\n",
           longest_stretch_consec_zero_samples, kLimitConsecFramesZero * channel_count_);
  }
}

}  // namespace media::audio::test
