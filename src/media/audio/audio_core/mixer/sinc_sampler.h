// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_AUDIO_CORE_MIXER_SINC_SAMPLER_H_
#define SRC_MEDIA_AUDIO_AUDIO_CORE_MIXER_SINC_SAMPLER_H_

#include <fuchsia/media/cpp/fidl.h>

#include "src/media/audio/audio_core/mixer/mixer.h"

namespace media::audio::mixer {

class SincSampler : public Mixer {
 public:
  static std::unique_ptr<Mixer> Select(const fuchsia::media::AudioStreamType& source_format,
                                       const fuchsia::media::AudioStreamType& dest_format);

 protected:
  SincSampler(Fixed pos_filter_width, Fixed neg_filter_width)
      : Mixer(pos_filter_width, neg_filter_width) {}
};

}  // namespace media::audio::mixer

#endif  // SRC_MEDIA_AUDIO_AUDIO_CORE_MIXER_SINC_SAMPLER_H_
