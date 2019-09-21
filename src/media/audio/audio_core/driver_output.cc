// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/audio_core/driver_output.h"

#include <lib/fit/defer.h>
#include <lib/zx/clock.h>

#include <iomanip>

#include <trace/event.h>

#include "src/media/audio/audio_core/audio_device_manager.h"
#include "src/media/audio/audio_core/reporter.h"

constexpr bool VERBOSE_TIMING_DEBUG = false;

namespace media::audio {

static constexpr uint32_t kDefaultFramesPerSec = 48000;
static constexpr uint32_t kDefaultChannelCount = 2;
static constexpr fuchsia::media::AudioSampleFormat kDefaultAudioFmt =
    fuchsia::media::AudioSampleFormat::SIGNED_24_IN_32;
// TODO(MTWN-269): Revert these to 20/30 instead of 50/60.
//                 In the long term, get these into the range of 5/10.
static constexpr zx_duration_t kDefaultLowWaterNsec = ZX_MSEC(50);
static constexpr zx_duration_t kDefaultHighWaterNsec = ZX_MSEC(60);
static constexpr zx_duration_t kDefaultMaxRetentionNsec = ZX_MSEC(60);
static constexpr zx_duration_t kDefaultRetentionGapNsec = ZX_MSEC(10);
static constexpr zx_duration_t kUnderflowCooldown = ZX_SEC(1);

static std::atomic<zx_txid_t> TXID_GEN(1);
static thread_local zx_txid_t TXID = TXID_GEN.fetch_add(1);

// Consts used if kEnableFinalMixWavWriter is set:
//
// This atomic is only used when the final-mix wave-writer is enabled --
// specifically to generate unique ids for each final-mix WAV file.
std::atomic<uint32_t> DriverOutput::final_mix_instance_num_(0u);
// WAV file location: FilePathName+final_mix_instance_num_+FileExtension
constexpr const char* kDefaultWavFilePathName = "/tmp/final_mix_";
constexpr const char* kWavFileExtension = ".wav";

fbl::RefPtr<AudioOutput> DriverOutput::Create(zx::channel stream_channel,
                                              AudioDeviceManager* manager) {
  return fbl::AdoptRef(new DriverOutput(manager, std::move(stream_channel)));
}

DriverOutput::DriverOutput(AudioDeviceManager* manager, zx::channel initial_stream_channel)
    : AudioOutput(manager), initial_stream_channel_(std::move(initial_stream_channel)) {}

DriverOutput::~DriverOutput() { wav_writer_.Close(); }

zx_status_t DriverOutput::Init() {
  TRACE_DURATION("audio", "DriverOutput::Init");
  FXL_DCHECK(state_ == State::Uninitialized);

  zx_status_t res = AudioOutput::Init();
  if (res != ZX_OK) {
    return res;
  }

  res = driver_->Init(std::move(initial_stream_channel_));
  if (res != ZX_OK) {
    FXL_PLOG(ERROR, res) << "Failed to initialize driver object";
    return res;
  }

  state_ = State::FormatsUnknown;
  return res;
}

void DriverOutput::OnWakeup() {
  TRACE_DURATION("audio", "DriverOutput::OnWakeup");
  // If we are not in the FormatsUnknown state, then we have already started the
  // state machine.  There is (currently) nothing else to do here.
  FXL_DCHECK(state_ != State::Uninitialized);
  if (state_ != State::FormatsUnknown) {
    return;
  }

  // Kick off the process of driver configuration by requesting the basic driver
  // info, which will include the modes which the driver supports.
  driver_->GetDriverInfo();
  state_ = State::FetchingFormats;
}

bool DriverOutput::StartMixJob(MixJob* job, fxl::TimePoint process_start) {
  TRACE_DURATION("audio", "DriverOutput::StartMixJob");
  if (state_ != State::Started) {
    FXL_LOG(ERROR) << "Bad state during StartMixJob " << static_cast<uint32_t>(state_);
    state_ = State::Shutdown;
    ShutdownSelf();
    return false;
  }

  // TODO(mpuryear): Depending on policy, use send appropriate commands to the
  // driver to control gain as well.  Some policy settings which might be useful
  // include...
  //
  // ++ Never use HW gain, even if it supports it.
  // ++ Always use HW gain when present, regardless of its limitations.
  // ++ Use HW gain when present, but only if it reaches a minimum bar of
  //    functionality.
  // ++ Implement a hybrid of HW/SW gain.  IOW - Get as close as possible to our
  //    target using HW, and then get the rest of the way there using SW
  //    scaling.  This approach may end up being unreasonably tricky as we may
  //    not be able to synchronize the HW and SW changes in gain well enough to
  //    avoid strange situations where the jumps in one direction (because of
  //    the SW component), and then in the other (as the HW gain command takes
  //    effect).
  //
  if (device_settings_ != nullptr) {
    AudioDeviceSettings::GainState cur_gain_state;
    device_settings_->SnapshotGainState(&cur_gain_state);
    job->sw_output_gain_db = cur_gain_state.gain_db;
    job->sw_output_muted = cur_gain_state.muted;
  } else {
    job->sw_output_gain_db = Gain::kUnityGainDb;
    //  TODO(mpuryear): make this false, consistent w/audio_device_settings.h?
    job->sw_output_muted = true;
  }

  FXL_DCHECK(driver_ring_buffer() != nullptr);
  auto uptime = zx::clock::get_monotonic().get();
  const auto& cm2rd_pos = clock_mono_to_ring_buf_pos_frames_;
  const auto& cm2frames = cm2rd_pos.rate();
  const auto& rb = *driver_ring_buffer();
  uint32_t fifo_frames = driver_->fifo_depth_frames();

  // If frames_to_mix_ is 0, then this is the start of a new mix. Ensure we have not underflowed
  // while sleeping, then compute how many frames to mix during this wakeup cycle, and return a job
  // containing the largest contiguous buffer we can mix during this phase of this cycle.
  if (!frames_to_mix_) {
    // output_frames_consumed is the number of frames that the audio output device has read so far.
    // output_frames_emitted is the slightly-smaller number of frames that have physically exited
    // the device itself (the number of frames that have "made sound" so far);
    int64_t output_frames_consumed = cm2rd_pos.Apply(uptime);
    int64_t output_frames_emitted = output_frames_consumed - fifo_frames;

    if (output_frames_consumed >= frames_sent_) {
      if (!underflow_start_time_) {
        // If this was the first time we missed our limit, log a message, mark the start time of the
        // underflow event, and fill our entire ring buffer with silence.
        int64_t output_underflow_frames = output_frames_consumed - frames_sent_;
        int64_t low_water_frames_underflow = output_underflow_frames + low_water_frames_;

        zx_duration_t output_underflow_duration =
            cm2frames.Inverse().Scale(output_underflow_frames);
        FXL_CHECK(output_underflow_duration >= 0);

        zx_duration_t output_variance_from_expected_wakeup =
            cm2frames.Inverse().Scale(low_water_frames_underflow);

        FXL_LOG(ERROR) << "OUTPUT UNDERFLOW: Missed mix target by (worst-case, expected) = ("
                       << std::setprecision(4)
                       << static_cast<double>(output_underflow_duration) / ZX_MSEC(1) << ", "
                       << output_variance_from_expected_wakeup / ZX_MSEC(1)
                       << ") ms. Cooling down for " << kUnderflowCooldown / ZX_MSEC(1)
                       << " milliseconds.";

        // Use our Reporter to log this to Cobalt, if enabled.
        REP(OutputUnderflow(output_underflow_duration, uptime));

        underflow_start_time_ = uptime;
        output_producer_->FillWithSilence(rb.virt(), rb.frames());
        zx_cache_flush(rb.virt(), rb.size(), ZX_CACHE_FLUSH_DATA);

        wav_writer_.Close();
      }

      // Regardless of whether this was the first or a subsequent underflow,
      // update the cooldown deadline (the time at which we will start producing
      // frames again, provided we don't underflow again)
      underflow_cooldown_deadline_ = zx_deadline_after(kUnderflowCooldown);
    }

    int64_t fill_target = cm2rd_pos.Apply(uptime + kDefaultHighWaterNsec);

    // Are we in the middle of an underflow cooldown? If so, check whether we have recovered yet.
    if (underflow_start_time_) {
      if (uptime < underflow_cooldown_deadline_) {
        // Looks like we have not recovered yet.  Pretend to have produced the
        // frames we were going to produce and schedule the next wakeup time.
        frames_sent_ = fill_target;
        ScheduleNextLowWaterWakeup();
        return false;
      } else {
        // Looks like we recovered.  Log and go back to mixing.
        FXL_LOG(WARNING) << "OUTPUT UNDERFLOW: Recovered after "
                         << (uptime - underflow_start_time_) / ZX_MSEC(1) << " ms.";
        underflow_start_time_ = 0;
        underflow_cooldown_deadline_ = 0;
      }
    }

    int64_t frames_in_flight = frames_sent_ - output_frames_emitted;
    FXL_DCHECK((frames_in_flight >= 0) && (frames_in_flight <= rb.frames()));
    FXL_DCHECK(frames_sent_ <= fill_target);
    int64_t desired_frames = fill_target - frames_sent_;

    // If we woke up too early to have any work to do, just get out now.
    if (desired_frames == 0) {
      return false;
    }

    uint32_t rb_space = rb.frames() - static_cast<uint32_t>(frames_in_flight);
    if (desired_frames > rb.frames()) {
      FXL_LOG(ERROR) << "OUTPUT UNDERFLOW: want to produce " << desired_frames
                     << " but the ring buffer is only " << rb.frames() << " frames long.";
      return false;
    }

    frames_to_mix_ = static_cast<uint32_t>(fbl::min<int64_t>(rb_space, desired_frames));
  }

  uint32_t to_mix = frames_to_mix_;
  uint32_t wr_ptr = frames_sent_ % rb.frames();
  uint32_t contig_space = rb.frames() - wr_ptr;

  if (to_mix > contig_space) {
    to_mix = contig_space;
  }

  job->buf = rb.virt() + (rb.frame_size() * wr_ptr);
  job->buf_frames = to_mix;
  job->start_pts_of = frames_sent_;
  job->local_to_output = &cm2rd_pos;
  job->local_to_output_gen = clock_mono_to_ring_buf_pos_id_.get();

  return true;
}

bool DriverOutput::FinishMixJob(const MixJob& job) {
  TRACE_DURATION("audio", "DriverOutput::FinishMixJob");
  const auto& rb = driver_ring_buffer();
  FXL_DCHECK(rb != nullptr);
  size_t buf_len = job.buf_frames * rb->frame_size();

  wav_writer_.Write(job.buf, buf_len);
  wav_writer_.UpdateHeader();
  zx_cache_flush(job.buf, buf_len, ZX_CACHE_FLUSH_DATA);

  if (VERBOSE_TIMING_DEBUG) {
    const auto& cm2rd_pos = clock_mono_to_ring_buf_pos_frames_;
    auto now = zx::clock::get_monotonic();
    int64_t output_frames_consumed = cm2rd_pos.Apply(now.get());
    int64_t playback_lead_start = frames_sent_ - output_frames_consumed;
    int64_t playback_lead_end = playback_lead_start + job.buf_frames;

    FXL_LOG(INFO) << "PLead [" << std::setw(4) << playback_lead_start << ", " << std::setw(4)
                  << playback_lead_end << "]";
  }

  FXL_DCHECK(frames_to_mix_ >= job.buf_frames);
  frames_sent_ += job.buf_frames;
  frames_to_mix_ -= job.buf_frames;

  if (!frames_to_mix_) {
    ScheduleNextLowWaterWakeup();
    return false;
  }

  return true;
}

void DriverOutput::ApplyGainLimits(fuchsia::media::AudioGainInfo* in_out_info, uint32_t set_flags) {
  TRACE_DURATION("audio", "DriverOutput::ApplyGainLimits");
  // See the comment at the start of StartMixJob.  The actual limits we set here
  // are going to eventually depend on what our HW gain control capabilities
  // are, and how we choose to apply them (based on policy)
  FXL_DCHECK(in_out_info != nullptr);

  // We do not currently allow more than unity gain for audio outputs.
  if (in_out_info->gain_db > 0.0) {
    in_out_info->gain_db = 0;
  }

  // Audio outputs should never support AGC
  in_out_info->flags &= ~(fuchsia::media::AudioGainInfoFlag_AgcEnabled);
}

void DriverOutput::ScheduleNextLowWaterWakeup() {
  TRACE_DURATION("audio", "DriverOutput::ScheduleNextLowWaterWakeup");
  // Schedule next callback for the low water mark behind the write pointer.
  const auto& cm2rd_pos = clock_mono_to_ring_buf_pos_frames_;
  int64_t low_water_frames = frames_sent_ - low_water_frames_;
  int64_t low_water_time = cm2rd_pos.ApplyInverse(low_water_frames);
  SetNextSchedTime(fxl::TimePoint::FromEpochDelta(fxl::TimeDelta::FromNanoseconds(low_water_time)));
}

void DriverOutput::OnDriverInfoFetched() {
  TRACE_DURATION("audio", "DriverOutput::OnDriverInfoFetched");
  auto cleanup = fit::defer([this]() FXL_NO_THREAD_SAFETY_ANALYSIS {
    state_ = State::Shutdown;
    ShutdownSelf();
  });

  if (state_ != State::FetchingFormats) {
    FXL_LOG(ERROR) << "Unexpected GetFormatsComplete while in state "
                   << static_cast<uint32_t>(state_);
    return;
  }

  zx_status_t res;

  // TODO(mpuryear): Use the best driver-supported format, not hardwired default
  uint32_t pref_fps = kDefaultFramesPerSec;
  uint32_t pref_chan = kDefaultChannelCount;
  fuchsia::media::AudioSampleFormat pref_fmt = kDefaultAudioFmt;
  zx_duration_t min_rb_duration =
      kDefaultHighWaterNsec + kDefaultMaxRetentionNsec + kDefaultRetentionGapNsec;

  res = SelectBestFormat(driver_->format_ranges(), &pref_fps, &pref_chan, &pref_fmt);

  if (res != ZX_OK) {
    FXL_LOG(ERROR) << "Output: cannot match a driver format to this request: " << pref_fps
                   << " Hz, " << pref_chan << "-channel, sample format 0x" << std::hex
                   << static_cast<uint32_t>(pref_fmt);
    return;
  }

  // TODO(mpuryear): Save to the hub the configured format for this output.

  TimelineRate ns_to_frames(pref_fps, ZX_SEC(1));
  int64_t retention_frames = ns_to_frames.Scale(kDefaultMaxRetentionNsec);
  FXL_DCHECK(retention_frames != TimelineRate::kOverflow);
  FXL_DCHECK(retention_frames <= std::numeric_limits<uint32_t>::max());
  driver_->SetEndFenceToStartFenceFrames(static_cast<uint32_t>(retention_frames));

  // Select our output producer
  fuchsia::media::AudioStreamTypePtr config(fuchsia::media::AudioStreamType::New());
  config->frames_per_second = pref_fps;
  config->channels = pref_chan;
  config->sample_format = pref_fmt;

  output_producer_ = OutputProducer::Select(config);
  if (!output_producer_) {
    FXL_LOG(ERROR) << "Output: OutputProducer cannot support this request: " << pref_fps << " Hz, "
                   << pref_chan << "-channel, sample format 0x" << std::hex
                   << static_cast<uint32_t>(pref_fmt);
    return;
  }

  // Start the process of configuring our driver
  res = driver_->Configure(pref_fps, pref_chan, pref_fmt, min_rb_duration);
  if (res != ZX_OK) {
    FXL_LOG(ERROR) << "Output: failed to configure driver for: " << pref_fps << " Hz, " << pref_chan
                   << "-channel, sample format 0x" << std::hex << static_cast<uint32_t>(pref_fmt)
                   << " (res " << std::dec << res << ")";
    return;
  }

  if constexpr (kEnableFinalMixWavWriter) {
    std::string file_name_ = kDefaultWavFilePathName;
    uint32_t instance_count = final_mix_instance_num_.fetch_add(1);
    file_name_ += (std::to_string(instance_count) + kWavFileExtension);
    wav_writer_.Initialize(file_name_.c_str(), pref_fmt, pref_chan, pref_fps,
                           driver_->bytes_per_frame() * 8 / pref_chan);
  }

  // Tell AudioDeviceManager we are ready to be an active audio device.
  ActivateSelf();

  // Success; now wait until configuration completes.
  state_ = State::Configuring;
  cleanup.cancel();
}

void DriverOutput::OnDriverConfigComplete() {
  TRACE_DURATION("audio", "DriverOutput::OnDriverConfigComplete");
  auto cleanup = fit::defer([this]() FXL_NO_THREAD_SAFETY_ANALYSIS {
    state_ = State::Shutdown;
    ShutdownSelf();
  });

  if (state_ != State::Configuring) {
    FXL_LOG(ERROR) << "Unexpected ConfigComplete while in state " << static_cast<uint32_t>(state_);
    return;
  }

  // Now that our driver is completely configured, we have all the info needed
  // to compute the minimum clock lead time requrirement for this output.
  int64_t fifo_depth_nsec =
      TimelineRate::Scale(driver_->fifo_depth_frames(), ZX_SEC(1), driver_->frames_per_sec());
  min_clock_lead_time_nsec_ =
      driver_->external_delay_nsec() + fifo_depth_nsec + kDefaultHighWaterNsec;

  // Fill our brand new ring buffer with silence
  FXL_CHECK(driver_ring_buffer() != nullptr);
  const auto& rb = *driver_ring_buffer();
  FXL_DCHECK(output_producer_ != nullptr);
  FXL_DCHECK(rb.virt() != nullptr);
  output_producer_->FillWithSilence(rb.virt(), rb.frames());

  // Set up the intermediate buffer at the AudioOutput level
  //
  // TODO(mpuryear): The intermediate buffer probably does not need to be as
  // large as the entire ring buffer.  Consider limiting this to be something
  // only slightly larger than a nominal mix job.
  SetupMixBuffer(rb.frames());

  // Start the ring buffer running
  //
  // TODO(mpuryear) : Don't actually start things up here.  We should start only
  // when we have clients with work to do, and we should stop when we have no
  // work to do.  See MTWN-5
  zx_status_t res = driver_->Start();
  if (res != ZX_OK) {
    FXL_PLOG(ERROR, res) << "Failed to start ring buffer";
    return;
  }

  // Start monitoring plug state.
  res = driver_->SetPlugDetectEnabled(true);
  if (res != ZX_OK) {
    FXL_PLOG(ERROR, res) << "Failed to enable plug detection";
    return;
  }

  // Success
  state_ = State::Starting;
  cleanup.cancel();
}

void DriverOutput::OnDriverStartComplete() {
  TRACE_DURATION("audio", "DriverOutput::OnDriverStartComplete");
  if (state_ != State::Starting) {
    FXL_LOG(ERROR) << "Unexpected StartComplete while in state " << static_cast<uint32_t>(state_);
    return;
  }

  // Compute the transformation from clock mono to the ring buffer read position
  // in frames, rounded up.  Then compute our low water mark (in frames) and
  // where we want to start mixing.  Finally kick off the mixing engine by
  // manually calling Process.
  uint32_t bytes_per_frame = driver_->bytes_per_frame();
  int64_t offset = static_cast<int64_t>(1) - bytes_per_frame;
  const TimelineFunction bytes_to_frames(0, offset, 1, bytes_per_frame);
  const TimelineFunction& t_bytes = driver_clock_mono_to_ring_pos_bytes();

  clock_mono_to_ring_buf_pos_frames_ = TimelineFunction::Compose(bytes_to_frames, t_bytes);
  clock_mono_to_ring_buf_pos_id_.Next();

  const TimelineFunction& trans = clock_mono_to_ring_buf_pos_frames_;
  uint32_t fd_frames = driver_->fifo_depth_frames();
  low_water_frames_ = fd_frames + trans.rate().Scale(kDefaultLowWaterNsec);
  frames_sent_ = low_water_frames_;
  frames_to_mix_ = 0;

  if (VERBOSE_TIMING_DEBUG) {
    FXL_LOG(INFO) << "Audio output: FIFO depth (" << fd_frames << " frames " << std::fixed
                  << std::setprecision(3) << trans.rate().Inverse().Scale(fd_frames) / 1000000.0
                  << " mSec) Low Water (" << low_water_frames_ << " frames " << std::fixed
                  << std::setprecision(3)
                  << trans.rate().Inverse().Scale(low_water_frames_) / 1000000.0 << " mSec)";
  }

  state_ = State::Started;
  Process();
}

void DriverOutput::OnDriverPlugStateChange(bool plugged, zx_time_t plug_time) {
  TRACE_DURATION("audio", "DriverOutput::OnDriverPlugStateChange");
  // Reflect this message to the AudioDeviceManager so it can deal with the plug
  // state change.
  manager_->ScheduleMainThreadTask(
      [manager = manager_, output = fbl::RefPtr(this), plugged, plug_time]() {
        manager->HandlePlugStateChange(std::move(output), plugged, plug_time);
      });
}

}  // namespace media::audio
