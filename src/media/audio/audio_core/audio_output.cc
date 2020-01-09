// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

#include "src/media/audio/audio_core/audio_output.h"

#include <lib/fit/defer.h>
#include <lib/zx/clock.h>

#include <limits>

#include <trace/event.h>

#include "src/media/audio/audio_core/audio_renderer_impl.h"
#include "src/media/audio/audio_core/mixer/mixer.h"
#include "src/media/audio/audio_core/mixer/no_op.h"
#include "src/media/audio/lib/logging/logging.h"

namespace media::audio {

static constexpr zx::duration kMaxTrimPeriod = zx::msec(10);

AudioOutput::AudioOutput(ThreadingModel* threading_model, DeviceRegistry* registry)
    : AudioDevice(Type::Output, threading_model, registry) {
  next_sched_time_ = async::Now(mix_domain().dispatcher());
  next_sched_time_known_ = true;
}

void AudioOutput::Process() {
  TRACE_DURATION("audio", "AudioOutput::Process");
  FX_CHECK(mix_stage_);
  auto now = async::Now(mix_domain().dispatcher());

  // At this point, we should always know when our implementation would like to be called to do some
  // mixing work next. If we do not know, then we should have already shut down.
  //
  // If the next sched time has not arrived yet, don't attempt to mix anything. Just trim the queues
  // and move on.
  FX_DCHECK(next_sched_time_known_);
  if (now >= next_sched_time_) {
    // Clear the flag. If the implementation does not set it during the cycle by calling
    // SetNextSchedTime, we consider it an error and shut down.
    next_sched_time_known_ = false;

    auto mix_frames = StartMixJob(now);
    if (mix_frames) {
      auto buf = mix_stage_->Mix(now, *mix_frames);
      FinishMixJob(*mix_frames, reinterpret_cast<float*>(buf.payload()));
    } else {
      mix_stage_->Trim(now);
    }
  }

  if (!next_sched_time_known_) {
    FX_LOGS(ERROR) << "Output failed to schedule next service time. Shutting down!";
    ShutdownSelf();
    return;
  }

  // Figure out when we should wake up to do more work again. No matter how long our implementation
  // wants to wait, we need to make sure to wake up and periodically trim our input queues.
  auto max_sched_time = now + kMaxTrimPeriod;
  if (next_sched_time_ > max_sched_time) {
    next_sched_time_ = max_sched_time;
  }
  zx_status_t status = mix_timer_.PostForTime(mix_domain().dispatcher(), next_sched_time_);
  if (status != ZX_OK) {
    FX_PLOGS(ERROR, status) << "Failed to schedule mix";
    ShutdownSelf();
  }
}

fit::result<std::unique_ptr<Mixer>, zx_status_t> AudioOutput::InitializeSourceLink(
    const AudioObject& source, fbl::RefPtr<Stream> stream) {
  TRACE_DURATION("audio", "AudioOutput::InitializeSourceLink");

  if (stream) {
    auto mixer = mix_stage_->AddInput(std::move(stream));
    const auto& settings = device_settings();
    if (settings != nullptr) {
      AudioDeviceSettings::GainState cur_gain_state;
      settings->SnapshotGainState(&cur_gain_state);

      mixer->bookkeeping().gain.SetDestGain(
          cur_gain_state.muted
              ? fuchsia::media::audio::MUTED_GAIN_DB
              : fbl::clamp(cur_gain_state.gain_db, Gain::kMinGainDb, Gain::kMaxGainDb));
    }
    return fit::ok(std::move(mixer));
  }

  return fit::ok(std::make_unique<audio::mixer::NoOp>());
}

void AudioOutput::CleanupSourceLink(const AudioObject& source, fbl::RefPtr<Stream> stream) {
  if (stream) {
    mix_stage_->RemoveInput(*stream);
  }
}

void AudioOutput::Cleanup() {
  AudioDevice::Cleanup();
  mix_timer_.Cancel();
}

}  // namespace media::audio
