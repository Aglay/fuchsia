// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_AUDIO_CORE_TESTING_FAKE_STREAM_H_
#define SRC_MEDIA_AUDIO_AUDIO_CORE_TESTING_FAKE_STREAM_H_

#include <lib/zx/clock.h>

#include "src/media/audio/audio_core/clock_reference.h"
#include "src/media/audio/audio_core/mixer/gain.h"
#include "src/media/audio/audio_core/stream.h"
#include "src/media/audio/audio_core/versioned_timeline_function.h"

namespace media::audio::testing {

class FakeStream : public ReadableStream {
 public:
  FakeStream(const Format& format, size_t max_buffer_size = PAGE_SIZE);

  void set_usage_mask(StreamUsageMask mask) { usage_mask_ = mask; }
  void set_gain_db(float gain_db) { gain_db_ = gain_db; }

  const fbl::RefPtr<VersionedTimelineFunction>& timeline_function() const {
    return timeline_function_;
  }

  // |media::audio::ReadableStream|
  std::optional<Buffer> ReadLock(zx::time ref_time, int64_t frame, uint32_t frame_count);
  void Trim(zx::time ref_time) {}
  TimelineFunctionSnapshot ReferenceClockToFractionalFrames() const;
  ClockReference reference_clock() const { return reference_clock_; }

 private:
  fbl::RefPtr<VersionedTimelineFunction> timeline_function_ =
      fbl::MakeRefCounted<VersionedTimelineFunction>();
  size_t buffer_size_;
  StreamUsageMask usage_mask_;
  float gain_db_ = Gain::kUnityGainDb;
  std::unique_ptr<uint8_t[]> buffer_;
  zx::clock clock_mono_;
  ClockReference reference_clock_;
};

}  // namespace media::audio::testing

#endif  // SRC_MEDIA_AUDIO_AUDIO_CORE_TESTING_FAKE_STREAM_H_
