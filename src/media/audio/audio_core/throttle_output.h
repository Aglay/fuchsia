// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_AUDIO_CORE_THROTTLE_OUTPUT_H_
#define SRC_MEDIA_AUDIO_AUDIO_CORE_THROTTLE_OUTPUT_H_

#include <fuchsia/media/cpp/fidl.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/zx/time.h>

#include <fbl/ref_ptr.h>

#include "src/lib/fxl/synchronization/thread_annotations.h"
#include "src/media/audio/audio_core/audio_output.h"
#include "src/media/audio/lib/clock/utils.h"

namespace media::audio {

static constexpr zx::duration TRIM_PERIOD = zx::msec(10);

// Throttle output may only be owned on the FIDL thread.
class ThrottleOutput : public AudioOutput {
 public:
  static std::shared_ptr<AudioOutput> Create(ThreadingModel* threading_model,
                                             DeviceRegistry* registry, LinkMatrix* link_matrix) {
    return std::make_shared<ThrottleOutput>(threading_model, registry, link_matrix);
  }

  ThrottleOutput(ThreadingModel* threading_model, DeviceRegistry* registry, LinkMatrix* link_matrix)
      : AudioOutput(threading_model, registry, link_matrix) {
    // This is just some placeholder format that we can use to instantiate a mix
    // stage for us. Since we never return a value from |StartMixJob|, we'll only
    // ever trim on this mix stage, so the format here is not particularly
    // important.
    //
    // Longer term we should just have something like a 'NullMixStage' that only
    // has this trim capability.

    // This must be non-0, but it doesn't actually matter much since we'll never mix with a throttle
    // output.
    static const uint32_t kMaxBatchSize = PAGE_SIZE;
    static PipelineConfig default_config = PipelineConfig::Default();
    SetupMixTask(default_config,
                 VolumeCurve::DefaultForMinGain(VolumeCurve::kDefaultGainForMinVolume),
                 kMaxBatchSize, TimelineFunction());
  }

  ~ThrottleOutput() override = default;

 protected:
  // AudioOutput Implementation
  void OnWakeup() FXL_EXCLUSIVE_LOCKS_REQUIRED(mix_domain().token()) override {
    if (uninitialized_) {
      last_sched_time_mono_ = async::Now(mix_domain().dispatcher());

      UpdatePlugState(true, zx::time(0));
      Process();
      uninitialized_ = false;
    }
  }

  std::optional<AudioOutput::FrameSpan> StartMixJob(zx::time ref_time) override {
    // Compute the next callback time; check whether trimming is falling behind.
    last_sched_time_mono_ = last_sched_time_mono_ + TRIM_PERIOD;
    auto mono_time = reference_clock().MonotonicTimeFromReferenceTime(ref_time).take_value();
    if (mono_time > last_sched_time_mono_) {
      // TODO(mpuryear): Trimming is falling behind. We should tell someone.
      last_sched_time_mono_ = mono_time + TRIM_PERIOD;
    }

    // TODO(mpuryear): Optimize the trim operation by scheduling callbacks for
    // when our first pending packet ends, rather than polling . This will also
    // tighten our timing in returning packets (currently, we hold packets up to
    // [TRIM_PERIOD-episilon] past their end PTS before releasing).
    //
    // To do this, we would need wake and recompute, whenever an AudioRenderer
    // client changes its rate transformation. For now, just polling is simpler.
    SetNextSchedTimeMono(last_sched_time_mono_);

    // Throttle outputs don't actually mix; they provide backpressure to the
    // pipeline by holding AudioPacket references until they are presented. We
    // only need to schedule our next callback to keep things running, and let
    // the base class implementation handle trimming the output.
    return std::nullopt;
  }

  void FinishMixJob(const AudioOutput::FrameSpan& span, float* buffer) override {
    // Since we never start any jobs, this should never be called.
    FX_DCHECK(false);
  }

  // AudioDevice implementation.
  // No one should ever be trying to apply gain limits for a throttle output.
  void ApplyGainLimits(fuchsia::media::AudioGainInfo* in_out_info,
                       fuchsia::media::AudioGainValidFlags set_flags) override {
    FX_DCHECK(false);
  }
  virtual zx::time last_sched_time_mono() { return last_sched_time_mono_; }

 private:
  zx::time last_sched_time_mono_;

  bool uninitialized_ = true;
};

}  // namespace media::audio

#endif  // SRC_MEDIA_AUDIO_AUDIO_CORE_THROTTLE_OUTPUT_H_
