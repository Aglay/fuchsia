// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_LIB_FORMAT_TRAITS_H_
#define SRC_MEDIA_AUDIO_LIB_FORMAT_TRAITS_H_

#include <fuchsia/media/cpp/fidl.h>
#include <fuchsia/virtualaudio/cpp/fidl.h>
#include <lib/fzl/vmo-mapper.h>
#include <lib/zx/vmo.h>

#include <cmath>
#include <memory>

#include "src/lib/fxl/strings/string_printf.h"
#include "src/media/audio/lib/format/constants.h"
#include "src/media/audio/lib/format/format.h"

namespace media::audio {

template <fuchsia::media::AudioSampleFormat SampleFormat>
struct SampleFormatTraits {
  // type SampleT = type of an individual sample
  // SampleT kSilentValue = when repeated, produces silent audio
  //
  // std::string ToString(SampleT) = convert from SampleT to text
  // float ToFloat(SampleT) = convert from SampleT to float
};

template <>
struct SampleFormatTraits<fuchsia::media::AudioSampleFormat::UNSIGNED_8> {
  using SampleT = uint8_t;
  static constexpr SampleT kSilentValue = 0x80;
  static std::string ToString(SampleT sample) { return fxl::StringPrintf("%02x", 0x0ff & sample); }
  static float ToFloat(SampleT sample) {
    return static_cast<float>(static_cast<int8_t>(sample - kSilentValue)) * kInt8ToFloat;
  }
};

template <>
struct SampleFormatTraits<fuchsia::media::AudioSampleFormat::SIGNED_16> {
  using SampleT = int16_t;
  static constexpr SampleT kSilentValue = 0;
  static std::string ToString(SampleT sample) { return fxl::StringPrintf("%04x", sample); }
  static float ToFloat(SampleT sample) { return static_cast<float>(sample) * kInt16ToFloat; }
};

template <>
struct SampleFormatTraits<fuchsia::media::AudioSampleFormat::SIGNED_24_IN_32> {
  using SampleT = int32_t;
  static constexpr SampleT kSilentValue = 0;
  static std::string ToString(SampleT sample) { return fxl::StringPrintf("%08x", sample); }
  static float ToFloat(SampleT sample) { return static_cast<float>(sample) * kInt24In32ToFloat; }
};

template <>
struct SampleFormatTraits<fuchsia::media::AudioSampleFormat::FLOAT> {
  using SampleT = float;
  static constexpr SampleT kSilentValue = 0;
  static std::string ToString(SampleT sample) { return fxl::StringPrintf("%.6f", sample); }
  static float ToFloat(SampleT sample) { return sample; }
};

}  // namespace media::audio

#endif  // SRC_MEDIA_AUDIO_LIB_FORMAT_TRAITS_H_
