// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_LIB_TEST_CAPTURER_SHIM_H_
#define SRC_MEDIA_AUDIO_LIB_TEST_CAPTURER_SHIM_H_

#include <fuchsia/media/cpp/fidl.h>
#include <fuchsia/ultrasound/cpp/fidl.h>

#include <memory>

#include "src/lib/testing/loop_fixture/real_loop_fixture.h"
#include "src/media/audio/lib/format/format.h"
#include "src/media/audio/lib/test/audio_buffer.h"
#include "src/media/audio/lib/test/test_fixture.h"
#include "src/media/audio/lib/test/vmo_backed_buffer.h"

namespace media::audio::test {

class CapturerShimImpl {
 public:
  static constexpr uint32_t kPacketMs = 10;

  ~CapturerShimImpl() {}

  fuchsia::media::AudioCapturerPtr& capturer() { return capturer_; }
  VmoBackedBuffer& payload() { return payload_buffer_; }

  size_t num_payload_frames() const { return payload_frame_count_; }
  size_t num_payload_samples() const { return payload_frame_count_ * format_.channels(); }
  size_t num_payload_bytes() const { return payload_frame_count_ * format_.bytes_per_frame(); }

 protected:
  CapturerShimImpl(Format format, size_t payload_frame_count)
      : format_(format),
        payload_frame_count_(payload_frame_count),
        payload_buffer_(format, payload_frame_count) {}

  void CreatePayloadBuffer();

  const Format format_;
  const size_t payload_frame_count_;

  fuchsia::media::AudioCapturerPtr capturer_;
  VmoBackedBuffer payload_buffer_;
};

template <fuchsia::media::AudioSampleFormat SampleFormat>
class AudioCapturerShim : public CapturerShimImpl {
 public:
  using SampleT = typename AudioBuffer<SampleFormat>::SampleT;

  // Snapshot the current payload.
  AudioBuffer<SampleFormat> SnapshotPayload() { return payload_buffer_.Snapshot<SampleFormat>(); }

  // Don't call this directly. Use HermeticAudioTest::CreateAudioCapturer so the object is
  // appropriately bound into the test environment.
  AudioCapturerShim(TestFixture* fixture, fuchsia::media::AudioCorePtr& audio_core, Format format,
                    size_t payload_frame_count, fuchsia::media::AudioCapturerConfiguration config)
      : CapturerShimImpl(format, payload_frame_count) {
    audio_core->CreateAudioCapturerWithConfiguration(format.stream_type(), std::move(config),
                                                     capturer_.NewRequest());
    capturer_.set_error_handler(fixture->ErrorHandler());

    capturer_->SetPcmStreamType({.sample_format = format_.sample_format(),
                                 .channels = format_.channels(),
                                 .frames_per_second = format_.frames_per_second()});
    capturer_->AddPayloadBuffer(0, payload_buffer_.CreateAndMapVmo(true));
  }
};

template <fuchsia::media::AudioSampleFormat SampleFormat>
class UltrasoundCapturerShim : public CapturerShimImpl {
 public:
  using SampleT = typename AudioBuffer<SampleFormat>::SampleT;

  const zx::clock& reference_clock() const { return reference_clock_; }

  // Snapshot the current payload.
  AudioBufferSlice<SampleFormat> SnapshotPayload() {
    return payload_buffer_.Snapshot<SampleFormat>();
  }

  // Don't call this directly. Use HermeticAudioTest::CreateUltrasoundCapturer so the object is
  // appropriately bound into the test environment.
  UltrasoundCapturerShim(TestFixture* fixture, fuchsia::ultrasound::FactoryPtr& ultrasound_factory,
                         Format format, size_t payload_frame_count)
      : CapturerShimImpl(format, payload_frame_count) {
    bool created = false;
    ultrasound_factory->CreateCapturer(
        capturer_.NewRequest(), [this, &created](auto ref_clock, auto stream_type) {
          created = true;
          reference_clock_ = std::move(ref_clock);
          EXPECT_EQ(stream_type.sample_format, format_.sample_format());
          EXPECT_EQ(stream_type.channels, format_.channels());
          EXPECT_EQ(stream_type.frames_per_second, format_.frames_per_second());
        });
    capturer_.set_error_handler(fixture->ErrorHandler());
    capturer_->AddPayloadBuffer(0, payload_buffer_.CreateAndMapVmo(true));
    fixture->RunLoopUntil([&created] { return created; });
  }

 private:
  zx::clock reference_clock_;
};

}  // namespace media::audio::test

#endif  // SRC_MEDIA_AUDIO_LIB_TEST_CAPTURER_SHIM_H_
