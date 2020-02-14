// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/audio_core/format.h"

#include "src/lib/syslog/cpp/logger.h"
#include "src/media/audio/audio_core/mixer/frames.h"

namespace media::audio {

Format::Format(fuchsia::media::AudioStreamType stream_type) : stream_type_(stream_type) {
  // Precompute some useful timing/format stuff.
  //
  // Start with the ratio between frames and nanoseconds.
  frames_per_ns_ = TimelineRate(stream_type_.frames_per_second, ZX_SEC(1));

  // Figure out the rate we need to scale by in order to produce our fixed point timestamps.
  frame_to_media_ratio_ = TimelineRate(FractionalFrames<int32_t>(1).raw_value(), 1);

  // Figure out the total number of bytes in a packed frame.
  switch (stream_type_.sample_format) {
    case fuchsia::media::AudioSampleFormat::UNSIGNED_8:
      bytes_per_frame_ = 1;
      break;

    case fuchsia::media::AudioSampleFormat::SIGNED_16:
      bytes_per_frame_ = 2;
      break;

    case fuchsia::media::AudioSampleFormat::SIGNED_24_IN_32:
    case fuchsia::media::AudioSampleFormat::FLOAT:
      bytes_per_frame_ = 4;
      break;

    default:
      // Format filtering was supposed to happen during AudioRendererImpl::SetStreamType.  It should
      // never be attempting to create a FormatInfo structure with a sample format that we do not
      // understand.
      FX_CHECK(false) << "unrecognized sample format";
      bytes_per_frame_ = 2;
      break;
  }

  bytes_per_frame_ *= stream_type_.channels;
}

bool Format::operator==(const Format& other) const {
  // All the other class members are derived from our stream_type, so we don't need to include them
  // here.
  return fidl::Equals(stream_type_, other.stream_type_);
}

// static
std::shared_ptr<Format> Format::Create(fuchsia::media::AudioStreamType format) {
  return std::make_shared<Format>(format);
}

}  // namespace media::audio
