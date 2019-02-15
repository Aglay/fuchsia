// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/media/cpp/fidl.h>

#include <lib/gtest/real_loop_fixture.h>
#include <cmath>

#include "gtest/gtest.h"
#include "lib/component/cpp/environment_services_helper.h"

namespace media::audio::test {

constexpr uint32_t kGainFlagMask =
    fuchsia::media::AudioGainInfoFlag_Mute |
    fuchsia::media::AudioGainInfoFlag_AgcSupported |
    fuchsia::media::AudioGainInfoFlag_AgcEnabled;
constexpr uint32_t kSetFlagMask = fuchsia::media::SetAudioGainFlag_GainValid |
                                  fuchsia::media::SetAudioGainFlag_MuteValid |
                                  fuchsia::media::SetAudioGainFlag_AgcValid;

constexpr uint16_t kInvalidDeviceCount = -1;
constexpr uint64_t kInvalidDeviceToken = -1;
constexpr fuchsia::media::AudioGainInfo kInvalidGainInfo = {
    .gain_db = NAN, .flags = ~kGainFlagMask};
const fuchsia::media::AudioDeviceInfo kInvalidDeviceInfo = {
    .name = "Invalid name",
    .unique_id = "Invalid unique_id (len 32 chars)",
    .token_id = kInvalidDeviceToken,
    .is_input = true,
    .gain_info = kInvalidGainInfo,
    .is_default = true};

class AudioDeviceTest : public gtest::RealLoopFixture {
 public:
  static void SetEnvironmentServices(
      std::shared_ptr<const ::component::Services> environment_services) {
    environment_services_ = environment_services;
  }

 protected:
  void SetUp() override;
  void TearDown() override;
  bool ExpectCallback();
  bool ExpectTimeout();

  void SetOnDeviceAddedEvent();
  void SetOnDeviceRemovedEvent();
  void SetOnDeviceGainChangedEvent();
  void SetOnDefaultDeviceChangedEvent();

  uint32_t GainFlagsFromBools(bool cur_mute, bool cur_agc, bool can_mute,
                              bool can_agc);
  uint32_t SetFlagsFromBools(bool set_gain, bool set_mute, bool set_agc);

  void RetrieveDefaultDevInfoUsingGetDevices(bool get_input);
  bool RetrieveGainInfoUsingGetDevices(uint64_t token);
  void RetrieveGainInfoUsingGetDeviceGain(uint64_t token,
                                          bool valid_token = true);
  void RetrieveTokenUsingGetDefault(bool is_input);
  void RetrievePreExistingDevices();
  bool HasPreExistingDevices();

  static uint16_t initial_input_device_count_;
  static uint16_t initial_output_device_count_;
  static uint64_t initial_input_default_;
  static uint64_t initial_output_default_;
  static float initial_input_gain_db_;
  static float initial_output_gain_db_;
  static uint32_t initial_input_gain_flags_;
  static uint32_t initial_output_gain_flags_;

  static std::shared_ptr<const ::component::Services> environment_services_;
  fuchsia::media::AudioDeviceEnumeratorPtr audio_dev_enum_;

  bool error_occurred_ = false;
  bool received_callback_ = false;
  fuchsia::media::AudioDeviceInfo received_device_ = kInvalidDeviceInfo;
  uint64_t received_removed_token_ = kInvalidDeviceToken;
  uint64_t received_gain_token_ = kInvalidDeviceToken;
  fuchsia::media::AudioGainInfo received_gain_info_ = kInvalidGainInfo;
  uint64_t received_default_token_ = kInvalidDeviceToken;
  uint64_t received_old_token_ = kInvalidDeviceToken;
};

}  // namespace media::audio::test
