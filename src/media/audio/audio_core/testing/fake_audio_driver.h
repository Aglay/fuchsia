// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_AUDIO_CORE_TESTING_FAKE_AUDIO_DRIVER_H_
#define SRC_MEDIA_AUDIO_AUDIO_CORE_TESTING_FAKE_AUDIO_DRIVER_H_

#include <fuchsia/hardware/audio/cpp/fidl.h>
#include <lib/async/cpp/time.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/fit/result.h>
#include <lib/fzl/vmo-mapper.h>
#include <lib/zx/channel.h>
#include <lib/zx/vmo.h>
#include <zircon/device/audio.h>

#include <cstring>
#include <optional>

#include "src/media/audio/lib/test/message_transceiver.h"

namespace media::audio::testing {

class FakeAudioDriverV1 {
 public:
  FakeAudioDriverV1(zx::channel channel, async_dispatcher_t* dispatcher);

  fzl::VmoMapper CreateRingBuffer(size_t size);

  // Starts an async wait that will process messages as they're received.
  void Start();

  // Cease processing messages as they're received.
  void Stop();

  // Processes a single message from the driver channel and returns the |audio_cmd_t| that was
  // processed.
  //
  // If there are no messages to process, ZX_ERR_SHOULD_WAIT is returned as an error.
  fit::result<audio_cmd_t, zx_status_t> Step();

  // Processes a single message from the driver ring buffer channel and returns the |audio_cmd_t|
  // that was processed.
  //
  // If there are no messages to process, ZX_ERR_SHOULD_WAIT is returned as an error.
  fit::result<audio_cmd_t, zx_status_t> StepRingBuffer();

  struct SelectedFormat {
    uint32_t frames_per_second;
    audio_sample_format_t sample_format;
    uint16_t channels;
  };

  void set_stream_unique_id(const audio_stream_unique_id_t& uid) {
    std::memcpy(uid_.data, uid.data, sizeof(uid.data));
  }
  void set_device_manufacturer(std::string mfgr) { manufacturer_ = std::move(mfgr); }
  void set_device_product(std::string product) { product_ = std::move(product); }
  void set_gain(float gain) { cur_gain_ = gain; }
  void set_gain_limits(float min_gain, float max_gain) {
    gain_limits_ = std::make_pair(min_gain, max_gain);
  }
  void set_can_agc(bool can_agc) { can_agc_ = can_agc; }
  void set_cur_agc(bool cur_agc) { cur_agc_ = cur_agc; }
  void set_can_mute(bool can_mute) { can_mute_ = can_mute; }
  void set_cur_mute(bool cur_mute) { cur_mute_ = cur_mute; }
  void set_formats(std::vector<audio_stream_format_range_t> formats) {
    formats_ = std::move(formats);
  }
  void set_clock_domain(uint32_t clock_domain) { clock_domain_ = clock_domain; }
  void set_hardwired(bool hardwired) { hardwired_ = hardwired; }
  void set_plugged(bool plugged) { plugged_ = plugged; }
  void set_fifo_depth(uint32_t fifo_depth) { fifo_depth_ = fifo_depth; }
  void set_external_delay(zx::duration external_delay) { external_delay_ = external_delay; }

  // |true| after an |audio_rb_cmd_start| is received, until an |audio_rb_cmd_stop| is received.
  bool is_running() const { return is_running_; }

  // The 'selected format' for the driver, chosen with a |AUDIO_STREAM_CMD_SET_FORMAT| command.
  //
  // The returned optional will be empty if no |AUDIO_STREAM_CMD_SET_FORMAT| command has been
  // received.
  std::optional<SelectedFormat> selected_format() const { return selected_format_; }

 private:
  void OnInboundStreamMessage(test::MessageTransceiver::Message message);
  void OnInboundStreamError(zx_status_t status);
  void HandleCommandGetUniqueId(const audio_stream_cmd_get_unique_id_req_t& request);
  void HandleCommandGetString(const audio_stream_cmd_get_string_req_t& request);
  void HandleCommandGetGain(const audio_stream_cmd_get_gain_req_t& request);
  void HandleCommandSetGain(const audio_stream_cmd_set_gain_req_t& request);
  void HandleCommandGetFormats(const audio_stream_cmd_get_formats_req_t& request);
  void HandleCommandSetFormat(const audio_stream_cmd_set_format_req_t& request);
  void HandleCommandPlugDetect(const audio_stream_cmd_plug_detect_req_t& request);
  void HandleCommandGetClockDomain(const audio_stream_cmd_get_clock_domain_req_t& request);

  void OnInboundRingBufferMessage(test::MessageTransceiver::Message message);
  void OnInboundRingBufferError(zx_status_t status);
  void HandleCommandGetFifoDepth(audio_rb_cmd_get_fifo_depth_req_t& request);
  void HandleCommandGetBuffer(audio_rb_cmd_get_buffer_req_t& request);
  void HandleCommandStart(audio_rb_cmd_start_req_t& request);
  void HandleCommandStop(audio_rb_cmd_stop_req_t& request);

  audio_stream_unique_id_t uid_ = {};
  std::string manufacturer_ = "default manufacturer";
  std::string product_ = "default product";
  float cur_gain_ = 0.0f;
  std::pair<float, float> gain_limits_{-160.0f, 3.0f};
  bool can_agc_ = true;
  bool cur_agc_ = false;
  bool can_mute_ = true;
  bool cur_mute_ = false;
  std::vector<audio_stream_format_range_t> formats_{{
      .sample_formats = AUDIO_SAMPLE_FORMAT_16BIT,
      .min_frames_per_second = 48000,
      .max_frames_per_second = 48000,
      .min_channels = 2,
      .max_channels = 2,
      .flags = ASF_RANGE_FLAG_FPS_48000_FAMILY,
  }};
  // fuchsia::hardware::audio::CLOCK_DOMAIN_MONOTONIC is not defined for AudioDriverV1 types.
  uint32_t clock_domain_ = 0;

  size_t ring_buffer_size_;
  zx::vmo ring_buffer_;

  uint32_t fifo_depth_ = 0;
  zx::duration external_delay_{zx::nsec(0)};
  bool hardwired_ = true;
  bool plugged_ = true;

  std::optional<SelectedFormat> selected_format_;

  bool is_running_ = false;

  async_dispatcher_t* dispatcher_;
  bool is_stopped_ = true;
  test::MessageTransceiver stream_transceiver_;
  test::MessageTransceiver ring_buffer_transceiver_;

  audio_cmd_t last_stream_command_ = 0;
  audio_cmd_t last_ring_buffer_command_ = 0;
};

class FakeAudioDriverV2 : public fuchsia::hardware::audio::StreamConfig,
                          public fuchsia::hardware::audio::RingBuffer {
 public:
  FakeAudioDriverV2(zx::channel channel, async_dispatcher_t* dispatcher);

  fzl::VmoMapper CreateRingBuffer(size_t size);
  void Start();
  void Stop();

  void set_stream_unique_id(const audio_stream_unique_id_t& uid) {
    std::memcpy(uid_.data, uid.data, sizeof(uid.data));
  }
  void set_device_manufacturer(std::string mfgr) { manufacturer_ = std::move(mfgr); }
  void set_device_product(std::string product) { product_ = std::move(product); }
  void set_gain(float gain) { cur_gain_ = gain; }
  void set_gain_limits(float min_gain, float max_gain) {
    gain_limits_ = std::make_pair(min_gain, max_gain);
  }
  void set_can_agc(bool can_agc) { can_agc_ = can_agc; }
  void set_cur_agc(bool cur_agc) { cur_agc_ = cur_agc; }
  void set_can_mute(bool can_mute) { can_mute_ = can_mute; }
  void set_cur_mute(bool cur_mute) { cur_mute_ = cur_mute; }
  void set_formats(fuchsia::hardware::audio::PcmSupportedFormats formats) {
    formats_ = std::move(formats);
  }
  void set_clock_domain(uint32_t clock_domain) { clock_domain_ = clock_domain; }
  void set_plugged(bool plugged) { plugged_ = plugged; }
  void set_fifo_depth(uint32_t fifo_depth) { fifo_depth_ = fifo_depth; }
  void set_external_delay(zx::duration external_delay) { external_delay_ = external_delay; }

  // |true| after an |audio_rb_cmd_start| is received, until an |audio_rb_cmd_stop| is received.
  bool is_running() const { return is_running_; }

  // The 'selected format' for the driver.
  // The returned optional will be empty if no |CreateRingBuffer| command has been received.
  std::optional<fuchsia::hardware::audio::PcmFormat> selected_format() const {
    return selected_format_;
  }

 private:
  // fuchsia hardware audio StreamConfig Interface
  void GetProperties(fuchsia::hardware::audio::StreamConfig::GetPropertiesCallback callback) final;
  void GetSupportedFormats(
      fuchsia::hardware::audio::StreamConfig::GetSupportedFormatsCallback callback) final;
  void CreateRingBuffer(
      fuchsia::hardware::audio::Format format,
      ::fidl::InterfaceRequest<fuchsia::hardware::audio::RingBuffer> ring_buffer) final;
  void WatchGainState(
      fuchsia::hardware::audio::StreamConfig::WatchGainStateCallback callback) final;
  void SetGain(fuchsia::hardware::audio::GainState target_state) final;
  void WatchPlugState(
      fuchsia::hardware::audio::StreamConfig::WatchPlugStateCallback callback) final;

  // fuchsia hardware audio RingBuffer Interface
  void GetProperties(fuchsia::hardware::audio::RingBuffer::GetPropertiesCallback callback) final;
  void WatchClockRecoveryPositionInfo(
      fuchsia::hardware::audio::RingBuffer::WatchClockRecoveryPositionInfoCallback callback) final;
  void GetVmo(uint32_t min_frames, uint32_t clock_recovery_notifications_per_ring,
              fuchsia::hardware::audio::RingBuffer::GetVmoCallback callback) final;
  void Start(fuchsia::hardware::audio::RingBuffer::StartCallback callback) final;
  void Stop(fuchsia::hardware::audio::RingBuffer::StopCallback callback) final;

  audio_stream_unique_id_t uid_ = {};
  std::string manufacturer_ = "default manufacturer";
  std::string product_ = "default product";
  float cur_gain_ = 0.0f;
  std::pair<float, float> gain_limits_{-160.0f, 3.0f};
  bool can_agc_ = true;
  bool cur_agc_ = false;
  bool can_mute_ = true;
  bool cur_mute_ = false;
  bool plug_state_sent_ = false;
  bool gain_state_sent_ = false;
  fuchsia::hardware::audio::PcmSupportedFormats formats_ = {};
  uint32_t clock_domain_ = fuchsia::hardware::audio::CLOCK_DOMAIN_MONOTONIC;
  size_t ring_buffer_size_;
  zx::vmo ring_buffer_;

  uint32_t fifo_depth_ = 0;
  zx::duration external_delay_{zx::nsec(0)};
  bool plugged_ = true;

  std::optional<fuchsia::hardware::audio::PcmFormat> selected_format_;

  bool is_running_ = false;

  async_dispatcher_t* dispatcher_;
  fidl::Binding<fuchsia::hardware::audio::StreamConfig> stream_binding_;
  std::optional<fidl::Binding<fuchsia::hardware::audio::RingBuffer>> ring_buffer_binding_;
  fidl::InterfaceRequest<fuchsia::hardware::audio::StreamConfig> stream_req_;
  fidl::InterfaceRequest<fuchsia::hardware::audio::RingBuffer> ring_buffer_req_;
};

}  // namespace media::audio::testing

#endif  // SRC_MEDIA_AUDIO_AUDIO_CORE_TESTING_FAKE_AUDIO_DRIVER_H_
