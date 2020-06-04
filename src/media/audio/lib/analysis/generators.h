// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_LIB_ANALYSIS_GENERATORS_H_
#define SRC_MEDIA_AUDIO_LIB_ANALYSIS_GENERATORS_H_

#include <cmath>
#include <memory>

#include "src/media/audio/lib/format/audio_buffer.h"
#include "src/media/audio/lib/wav/wav_reader.h"

namespace media::audio {

// Construct a stream of silent audio data.
template <fuchsia::media::AudioSampleFormat SampleFormat>
AudioBuffer<SampleFormat> GenerateSilentAudio(Format format, size_t num_frames) {
  AudioBuffer<SampleFormat> buf(format, num_frames);
  std::fill(buf.samples().begin(), buf.samples().end(),
            SampleFormatTraits<SampleFormat>::kSilentValue);
  return buf;
}

// Construct a stream of synthetic audio data that is sequentially incremented. For integer types,
// payload data values increase by 1. For FLOAT, data increases by 2^-16, which is about 10^-5.
//
// As this does not create a meaningful sound, this is intended to be used in test scenarios that
// perform bit-for-bit comparisons on the output of an audio pipeline.
template <fuchsia::media::AudioSampleFormat SampleFormat>
AudioBuffer<SampleFormat> GenerateSequentialAudio(
    Format format, size_t num_frames, typename AudioBuffer<SampleFormat>::SampleT first_val = 0) {
  typename AudioBuffer<SampleFormat>::SampleT increment = 1;
  if (SampleFormat == fuchsia::media::AudioSampleFormat::FLOAT) {
    increment = pow(2.0, -16);
  }
  AudioBuffer<SampleFormat> out(format, num_frames);
  for (size_t sample = 0; sample < out.samples().size(); ++sample) {
    out.samples()[sample] = first_val;
    first_val += increment;
    if (SampleFormat == fuchsia::media::AudioSampleFormat::FLOAT && first_val > 1) {
      first_val = -1;
    }
  }
  return out;
}

// Construct a stream of sinusoidal values of the given number of frames, determined by equation
// "buffer[idx] = magn * cosine(idx*freq/num_frames*2*M_PI + phase)". If the format has >1 channels,
// each channel is assigned a duplicate value.
//
// Restated: |freq| is the number of **complete sinusoidal periods** that should perfectly fit into
// the buffer; |magn| is a multiplier applied to the output (default value is 1.0); |phase| is an
// offset (default value 0.0) which shifts the signal along the x-axis (value expressed in radians,
// so runs from -M_PI to +M_PI).
template <fuchsia::media::AudioSampleFormat SampleFormat>
AudioBuffer<SampleFormat> GenerateCosineAudio(Format format, size_t num_frames, double freq,
                                              double magn = 1.0, double phase = 0.0) {
  // If frequency is 0 (constant val), phase offset causes reduced amplitude
  FX_CHECK(freq > 0.0 || (freq == 0.0 && phase == 0.0));

  // Freqs above num_frames/2 (Nyquist limit) will alias into lower frequencies.
  FX_CHECK(freq * 2.0 <= num_frames) << "Buffer too short--requested frequency will be aliased";

  // freq is defined as: cosine recurs exactly 'freq' times within buf_size.
  const double mult = 2.0 * M_PI / num_frames * freq;

  AudioBuffer<SampleFormat> out(format, num_frames);
  for (size_t frame = 0; frame < num_frames; ++frame) {
    auto val = magn * std::cos(mult * frame + phase);
    switch (SampleFormat) {
      case fuchsia::media::AudioSampleFormat::UNSIGNED_8:
        val = round(val) + 0x80;
        break;
      case fuchsia::media::AudioSampleFormat::SIGNED_16:
      case fuchsia::media::AudioSampleFormat::SIGNED_24_IN_32:
        val = round(val);
        break;
      case fuchsia::media::AudioSampleFormat::FLOAT:
        break;
    }
    for (size_t chan = 0; chan < format.channels(); chan++) {
      out.samples()[out.SampleIndex(frame, chan)] = val;
    }
  }
  return out;
}

// Load audio from a WAV file.
template <fuchsia::media::AudioSampleFormat SampleFormat>
AudioBuffer<SampleFormat> LoadWavFile(const std::string& file_name) {
  auto r = WavReader::Open(file_name).take_value();
  auto format = Format::Create({
      .sample_format = r->sample_format(),
      .channels = r->channel_count(),
      .frames_per_second = r->frame_rate(),
  });

  AudioBuffer<SampleFormat> out(format.value(), r->length_in_frames());
  auto size = r->length_in_bytes();
  auto result = r->Read(&out.samples()[0], size);
  FX_CHECK(result.is_ok()) << "Read(" << file_name << ") failed, error: " << result.error();
  FX_CHECK(size == result.value())
      << "Read(" << file_name << ") failed, expected " << size << " bytes, got " << result.value();
  return out;
}

}  // namespace media::audio

#endif  // SRC_MEDIA_AUDIO_LIB_ANALYSIS_GENERATORS_H_
