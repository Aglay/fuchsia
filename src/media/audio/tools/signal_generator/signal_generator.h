// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_TOOLS_SIGNAL_GENERATOR_SIGNAL_GENERATOR_H_
#define SRC_MEDIA_AUDIO_TOOLS_SIGNAL_GENERATOR_SIGNAL_GENERATOR_H_

#include <fuchsia/media/cpp/fidl.h>
#include <lib/fit/function.h>
#include <lib/fzl/vmo-mapper.h>
#include <lib/sys/cpp/component_context.h>
#include <lib/zx/clock.h>

#include "src/media/audio/lib/wav_writer/wav_writer.h"

namespace {
constexpr float kUnityGainDb = 0.0f;

typedef enum {
  kOutputTypeNoise,
  kOutputTypePinkNoise,
  kOutputTypeSine,
  kOutputTypeSquare,
  kOutputTypeSawtooth,
  kOutputTypeRamp,
} OutputSignalType;
// TODO(49220): refactor signal-generation to make it easier for new generators to be added.

typedef enum { Default, Optimal, Monotonic, Custom } ClockType;

constexpr std::array<std::pair<const char*, fuchsia::media::AudioRenderUsage>,
                     fuchsia::media::RENDER_USAGE_COUNT>
    kRenderUsageOptions = {{
        {"BACKGROUND", fuchsia::media::AudioRenderUsage::BACKGROUND},
        {"MEDIA", fuchsia::media::AudioRenderUsage::MEDIA},
        {"INTERRUPTION", fuchsia::media::AudioRenderUsage::INTERRUPTION},
        {"SYSTEM_AGENT", fuchsia::media::AudioRenderUsage::SYSTEM_AGENT},
        {"COMMUNICATION", fuchsia::media::AudioRenderUsage::COMMUNICATION},
    }};

// Any audio output device fed by the system audio mixer will have this min_lead_time, at least.
// Until then, we cannot be confident that our renderer is routed to an actual device.
// TODO(50117): remove this workaround, once the underlying fxb/50017 is fixed in audio_core.
constexpr zx::duration kRealDeviceMinLeadTime = zx::msec(1);
}  // namespace

namespace media::tools {
class MediaApp {
 public:
  MediaApp(fit::closure quit_callback);

  void set_num_channels(uint32_t num_channels) { num_channels_ = num_channels; }
  void set_frame_rate(uint32_t frame_rate) { frame_rate_ = frame_rate; }
  void set_sample_format(fuchsia::media::AudioSampleFormat format) { sample_format_ = format; }

  void set_output_type(OutputSignalType output_type) { output_signal_type_ = output_type; }
  void set_usage(fuchsia::media::AudioRenderUsage usage) { usage_ = usage; }
  void set_frequency(double frequency) { frequency_ = frequency; }
  void set_amplitude(float amplitude) { amplitude_ = amplitude; }

  void set_duration(double duration_secs) { duration_secs_ = duration_secs; }
  double get_duration() { return duration_secs_; }

  void set_frames_per_payload(uint32_t frames_per_payload) {
    frames_per_payload_ = frames_per_payload;
  }
  void set_num_payload_buffers(uint32_t num_payload_buffers) {
    num_payload_buffers_ = num_payload_buffers;
  }

  void set_clock_type(ClockType type) { clock_type_ = type; }
  void adjust_clock_rate(int32_t rate_adjustment) {
    adjusting_clock_rate_ = true;
    clock_rate_adjustment_ = rate_adjustment;
  }

  void set_use_pts(bool use_pts) { use_pts_ = use_pts; }
  void set_pts_continuity_threshold(float pts_continuity_threshold) {
    set_continuity_threshold_ = true;
    pts_continuity_threshold_secs_ = pts_continuity_threshold;
  }

  void set_save_to_file(bool save_to_file) { save_to_file_ = save_to_file; }
  void set_save_file_name(std::string file_name) { file_name_ = file_name; }

  void set_stream_gain(float gain_db) {
    set_stream_gain_ = true;
    stream_gain_db_ = gain_db;
  }
  void set_stream_mute(bool mute) {
    set_stream_mute_ = true;
    stream_mute_ = mute;
  }

  void set_will_ramp_stream_gain() { ramp_stream_gain_ = true; }
  void set_ramp_duration_nsec(zx_duration_t duration_nsec) { ramp_duration_nsec_ = duration_nsec; }
  void set_ramp_target_gain_db(float gain_db) { ramp_target_gain_db_ = gain_db; }

  void set_usage_gain(float gain_db) {
    set_usage_gain_ = true;
    usage_gain_db_ = gain_db;
  }
  void set_usage_volume(float volume) {
    set_usage_volume_ = true;
    usage_volume_ = volume;
  }

  void set_verbose(bool verbose) { verbose_ = verbose; }
  void set_ultrasound(bool ultrasound) { ultrasound_ = ultrasound; }

  void Run(sys::ComponentContext* app_context);

 private:
  void ParameterRangeChecks();
  void SetupPayloadCoefficients();
  void DisplayConfigurationSettings();

  void SetAudioCoreSettings(sys::ComponentContext* app_context);
  void AcquireAudioRenderer(sys::ComponentContext* app_context);
  void SetAudioRendererEvents();
  void InitializeAudibleRenderer();
  void ConfigureAudioRendererPts();
  void InitializeWavWriter();
  void CreateMemoryMapping();

  void GetClockAndStart();
  void Play();

  void SendPacket();
  void OnSendPacketComplete(uint64_t frames_completed);

  struct AudioPacket {
    fuchsia::media::StreamPacket stream_packet;
    fzl::VmoMapper* vmo;
  };
  AudioPacket CreateAudioPacket(uint64_t packet_num);

  void PrimePinkNoiseFilter();
  void AdvancePinkNoiseFrame();
  double NextPinkNoiseSample(uint32_t chan_num);

  void GenerateAudioForPacket(const AudioPacket& packet, uint64_t packet_num);
  template <typename SampleType>
  void WriteAudioIntoBuffer(SampleType* audio_buffer, uint32_t num_frames,
                            uint64_t frames_since_start);

  bool Shutdown();
  fit::closure quit_callback_;

  fuchsia::media::AudioRendererPtr audio_renderer_;
  fuchsia::media::audio::GainControlPtr gain_control_;
  bool received_min_lead_time_ = false;
  zx::duration min_lead_time_;

  std::vector<fzl::VmoMapper> payload_buffers_;

  uint32_t num_channels_;
  uint32_t frame_rate_;
  fuchsia::media::AudioSampleFormat sample_format_ = fuchsia::media::AudioSampleFormat::FLOAT;
  uint32_t sample_size_;
  uint32_t frame_size_;

  fuchsia::media::AudioRenderUsage usage_ = fuchsia::media::AudioRenderUsage::MEDIA;
  fuchsia::media::audio::VolumeControlPtr usage_volume_control_;  // for usage volume

  OutputSignalType output_signal_type_;

  double frequency_;
  double frames_per_period_;  // frame_rate_ / frequency_

  double amplitude_;         // Amplitude between 0.0 and 1.0 (full-scale).
  double amplitude_scalar_;  // Amp translated to container-specific magn.

  double duration_secs_;
  uint32_t frames_per_payload_;
  uint32_t num_payload_buffers_;

  zx::clock reference_clock_;
  ClockType clock_type_ = ClockType::Default;
  bool adjusting_clock_rate_ = false;
  int32_t clock_rate_adjustment_ = 0;

  zx::time reference_start_time_;
  zx::time media_start_time_;
  bool use_pts_ = false;
  bool set_continuity_threshold_ = false;
  float pts_continuity_threshold_secs_;

  uint32_t payload_mapping_size_;
  uint32_t payload_size_;
  uint32_t payloads_per_mapping_;
  uint32_t total_num_mapped_payloads_;

  uint64_t total_frames_to_send_;
  uint64_t num_packets_to_send_;
  uint64_t num_packets_sent_ = 0u;
  uint64_t num_packets_completed_ = 0u;
  uint64_t num_frames_sent_ = 0u;
  uint64_t num_frames_completed_ = 0u;

  bool save_to_file_ = false;
  std::string file_name_;
  media::audio::WavWriter<> wav_writer_;
  bool wav_writer_initialized_ = false;

  bool set_stream_gain_ = false;
  float stream_gain_db_ = kUnityGainDb;
  bool set_stream_mute_ = false;
  bool stream_mute_ = false;

  bool ramp_stream_gain_ = false;
  float ramp_target_gain_db_ = kUnityGainDb;
  zx_duration_t ramp_duration_nsec_;

  bool set_usage_gain_ = false;
  float usage_gain_db_ = kUnityGainDb;
  bool set_usage_volume_ = false;
  float usage_volume_;

  bool verbose_ = false;

  // This 4-stage feedforward/feedback filter attenuates by 1/f to convert white noise to pink.
  static constexpr double kFeedFwd[] = {0.049922035, -0.095993537, 0.050612699, -0.004408786};
  static constexpr double kFeedBack[] = {1, -2.494956002, 2.017265875, -0.522189400};
  typedef double HistoryBuffer[4];
  std::unique_ptr<HistoryBuffer[]> input_history_;
  std::unique_ptr<HistoryBuffer[]> output_history_;

  bool ultrasound_ = false;
};

}  // namespace media::tools

#endif  // SRC_MEDIA_AUDIO_TOOLS_SIGNAL_GENERATOR_SIGNAL_GENERATOR_H_
