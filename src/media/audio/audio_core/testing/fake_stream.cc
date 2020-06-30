// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/audio_core/testing/fake_stream.h"

#include <lib/syslog/cpp/macros.h>

#include "src/media/audio/lib/clock/clone_mono.h"

namespace media::audio::testing {

FakeStream::FakeStream(const Format& format, size_t max_buffer_size)
    : ReadableStream(format),
      clock_mono_(clock::CloneOfMonotonic()),
      reference_clock_(ClockReference::MakeReadonly(clock_mono_)) {
  buffer_size_ = max_buffer_size;
  buffer_ = std::make_unique<uint8_t[]>(buffer_size_);
  memset(buffer_.get(), 0, buffer_size_);
}

std::optional<ReadableStream::Buffer> FakeStream::ReadLock(zx::time dest_ref_time, int64_t frame,
                                                           uint32_t frame_count) {
  FX_CHECK(frame_count * format().bytes_per_frame() < buffer_size_);
  return std::make_optional<ReadableStream::Buffer>(frame, frame_count, buffer_.get(), true,
                                                    usage_mask_, gain_db_);
}

ReadableStream::TimelineFunctionSnapshot FakeStream::ReferenceClockToFractionalFrames() const {
  auto [timeline_function, generation] = timeline_function_->get();
  return {
      .timeline_function = timeline_function,
      .generation = generation,
  };
}

}  // namespace media::audio::testing
