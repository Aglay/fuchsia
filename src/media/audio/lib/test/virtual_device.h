// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_LIB_TEST_VIRTUAL_DEVICE_H_
#define SRC_MEDIA_AUDIO_LIB_TEST_VIRTUAL_DEVICE_H_

#include <fuchsia/media/cpp/fidl.h>
#include <fuchsia/virtualaudio/cpp/fidl.h>
#include <lib/fzl/vmo-mapper.h>
#include <lib/zx/vmo.h>
#include <zircon/device/audio.h>

#include <memory>

#include "src/lib/testing/loop_fixture/real_loop_fixture.h"
#include "src/media/audio/lib/format/audio_buffer.h"
#include "src/media/audio/lib/test/hermetic_audio_environment.h"
#include "src/media/audio/lib/test/inspect.h"
#include "src/media/audio/lib/test/test_fixture.h"
#include "src/media/audio/lib/test/vmo_backed_buffer.h"

namespace media::audio::test {

namespace internal {
// These IDs are scoped to the lifetime of this process.
extern size_t virtual_output_next_inspect_id;
extern size_t virtual_input_next_inspect_id;
}  // namespace internal

// This class is thread hostile: none of its methods can be called concurrently.
template <class Interface>
class VirtualDevice {
 public:
  static constexpr uint32_t kNotifyMs = 10;
  static constexpr uint32_t kFifoDepthBytes = 0;
  static constexpr auto kExternalDelay = zx::msec(0);

  ~VirtualDevice();

  fidl::InterfacePtr<Interface>& virtual_device() { return device_; }
  size_t frame_count() const { return frame_count_; }

  // Reports whether the device has started.
  bool Ready() const { return received_start_; }

  // Returns a timestamp in the future that corresponds to byte 0 of the ring buffer.
  // This is a reference time that can be passed to AudioRenderer::Play().
  // The fixture is used to loop through time to locate the boundaries of the ring buffer.
  int64_t NextSynchronizedTimestamp(TestFixture* fixture) const;

  // For validating properties exported by inspect.
  // By default, there are no expectations.
  size_t inspect_id() const { return inspect_id_; }
  ExpectedInspectProperties& expected_inspect_properties() { return expected_inspect_properties_; }

 protected:
  VirtualDevice(TestFixture* fixture, HermeticAudioEnvironment* environment,
                const audio_stream_unique_id_t& device_id, Format format, size_t frame_count,
                size_t inspect_id);

  void ResetEvents();
  void WatchEvents();

  const Format format_;
  const size_t frame_count_;
  const size_t inspect_id_;

  fidl::InterfacePtr<Interface> device_;
  audio_sample_format_t driver_format_;
  zx::vmo rb_vmo_;
  VmoBackedBuffer rb_;
  bool received_set_format_ = false;
  bool received_start_ = false;
  bool received_stop_ = false;
  zx_time_t start_time_ = 0;
  zx_time_t stop_time_ = 0;
  uint64_t stop_pos_ = 0;
  uint64_t ring_pos_ = 0;
  uint64_t running_ring_pos_ = 0;

  ExpectedInspectProperties expected_inspect_properties_;
};

template <fuchsia::media::AudioSampleFormat SampleFormat>
class VirtualOutput : public VirtualDevice<fuchsia::virtualaudio::Output> {
 public:
  using SampleT = typename AudioBuffer<SampleFormat>::SampleT;

  // Take a snapshot of the device's ring buffer.
  AudioBuffer<SampleFormat> SnapshotRingBuffer() { return rb_.Snapshot<SampleFormat>(); }

  // Don't call this directly. Use HermeticAudioTest::CreateOutput so the object is
  // appropriately bound into the test environment.
  VirtualOutput(TestFixture* fixture, HermeticAudioEnvironment* environment,
                const audio_stream_unique_id_t& device_id, Format format, size_t frame_count)
      : VirtualDevice(fixture, environment, device_id, format, frame_count,
                      internal::virtual_output_next_inspect_id++) {}
};

template <fuchsia::media::AudioSampleFormat SampleFormat>
class VirtualInput : public VirtualDevice<fuchsia::virtualaudio::Input> {
 public:
  using SampleT = typename AudioBuffer<SampleFormat>::SampleT;

  // Write a slice to the ring buffer at the given position.
  void WriteRingBufferAt(size_t ring_pos_in_frames, AudioBufferSlice<SampleFormat> slice) {
    rb_.WriteAt<SampleFormat>(ring_pos_in_frames, slice);
  }

  // Don't call this directly. Use HermeticAudioTest::CreateInput so the object is
  // appropriately bound into the test environment.
  VirtualInput(TestFixture* fixture, HermeticAudioEnvironment* environment,
               const audio_stream_unique_id_t& device_id, Format format, size_t frame_count)
      : VirtualDevice(fixture, environment, device_id, format, frame_count,
                      internal::virtual_input_next_inspect_id++) {}
};

}  // namespace media::audio::test

#endif  // SRC_MEDIA_AUDIO_LIB_TEST_VIRTUAL_DEVICE_H_
