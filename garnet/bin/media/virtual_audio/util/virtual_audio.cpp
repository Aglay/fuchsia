// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/virtualaudio/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/component/cpp/startup_context.h>
#include <lib/fsl/tasks/fd_waiter.h>
#include <lib/fxl/command_line.h>
#include <lib/fxl/strings/string_number_conversions.h>
#include <lib/fzl/vmo-mapper.h>
#include <zircon/device/audio.h>

#include "garnet/drivers/audio/virtual_audio/virtual_audio.h"

namespace virtual_audio {

class VirtualAudioUtil {
 public:
  VirtualAudioUtil(async::Loop* loop) : loop_(loop) {}

  void Run(fxl::CommandLine* cmdline);

 private:
  enum class Command {
    ENABLE_VIRTUAL_AUDIO,
    DISABLE_VIRTUAL_AUDIO,

    SET_DEVICE_NAME,
    SET_MANUFACTURER,
    SET_PRODUCT_NAME,
    SET_UNIQUE_ID,
    ADD_FORMAT_RANGE,
    SET_FIFO_DEPTH,
    SET_EXTERNAL_DELAY,
    SET_RING_BUFFER_RESTRICTIONS,
    SET_GAIN_PROPS,
    SET_PLUG_PROPS,
    RESET_CONFIG,

    ADD_DEVICE,
    REMOVE_DEVICE,
    PLUG,
    UNPLUG,

    SET_IN,
    SET_OUT,
    WAIT,
    INVALID,
  };

  static constexpr struct {
    const char* name;
    Command cmd;
  } COMMANDS[] = {
      {"enable", Command::ENABLE_VIRTUAL_AUDIO},
      {"disable", Command::DISABLE_VIRTUAL_AUDIO},

      {"dev", Command::SET_DEVICE_NAME},
      {"mfg", Command::SET_MANUFACTURER},
      {"prod", Command::SET_PRODUCT_NAME},
      {"id", Command::SET_UNIQUE_ID},
      {"add-format", Command::ADD_FORMAT_RANGE},
      {"fifo", Command::SET_FIFO_DEPTH},
      {"delay", Command::SET_EXTERNAL_DELAY},
      {"rb", Command::SET_RING_BUFFER_RESTRICTIONS},
      {"gain-props", Command::SET_GAIN_PROPS},
      {"plug-props", Command::SET_PLUG_PROPS},
      {"reset", Command::RESET_CONFIG},

      {"add", Command::ADD_DEVICE},
      {"remove", Command::REMOVE_DEVICE},

      {"plug", Command::PLUG},
      {"unplug", Command::UNPLUG},

      {"in", Command::SET_IN},
      {"out", Command::SET_OUT},
      {"wait", Command::WAIT},
  };
  static constexpr char kDefaultDeviceName[] = "Vertex";
  static constexpr char kDefaultManufacturer[] =
      "Puerile Virtual Functions, Incorporated";
  static constexpr char kDefaultProductName[] = "Virgil, version 1.0";
  static constexpr uint8_t kDefaultUniqueId[16] = {
      0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
      0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};

  static constexpr uint8_t kDefaultFormatRangeOption = 0;

  static constexpr uint32_t kDefaultFifoDepth = 0x100;
  static constexpr uint64_t kDefaultExternalDelayNsec = ZX_MSEC(1);
  static constexpr uint8_t kDefaultRingBufferOption = 0;

  static constexpr uint8_t kDefaultGainPropsOption = 0;
  static constexpr uint8_t kDefaultPlugPropsOption = 0;

  void QuitLoop();
  bool RunLoopWithTimeout(zx::duration timeout);
  bool WaitForNoCallback();
  bool WaitForCallback();

  void RegisterKeyWaiter();
  bool WaitForKey();

  void ConnectToController();
  bool ConnectToDevice();
  bool ConnectToInput();
  bool ConnectToOutput();

  void ParseAndExecute(fxl::CommandLine* cmdline);
  bool ExecuteCommand(Command cmd, const std::string& value);

  // Methods using the FIDL Service interface
  bool Enable(bool enable);

  // Methods using the FIDL Configuration interface
  bool SetDeviceName(const std::string& name);
  bool SetManufacturer(const std::string& name);
  bool SetProductName(const std::string& name);
  bool SetUniqueId(const std::string& unique_id);
  bool AddFormatRange(const std::string& format_str);
  bool SetFifoDepth(const std::string& fifo_str);
  bool SetExternalDelay(const std::string& delay_str);
  bool SetRingBufferRestrictions(const std::string& rb_restr_str);
  bool SetGainProperties(const std::string& gain_props_str);
  bool SetPlugProperties(const std::string& plug_props_str);
  bool ResetConfiguration();

  bool AddDevice();

  // Methods using the FIDL Device interface
  bool RemoveDevice();
  bool ChangePlugState(const std::string& plug_time_str, bool plugged);

  std::unique_ptr<component::StartupContext> startup_context_;
  ::async::Loop* loop_;
  fsl::FDWaiter keystroke_waiter_;
  bool key_quit_ = false;

  fuchsia::virtualaudio::ControlSyncPtr controller_ = nullptr;
  fuchsia::virtualaudio::InputPtr input_ = nullptr;
  fuchsia::virtualaudio::OutputPtr output_ = nullptr;

  bool configuring_output_ = true;
};

// VirtualAudioUtil implementation
//
void VirtualAudioUtil::Run(fxl::CommandLine* cmdline) {
  ParseAndExecute(cmdline);

  // We are done!  Disconnect any error handlers.
  if (input_.is_bound()) {
    input_.set_error_handler(nullptr);
  }
  if (output_.is_bound()) {
    output_.set_error_handler(nullptr);
  }

  printf("\n");
  // If any lingering callbacks were queued, let them drain.
  if (!RunLoopWithTimeout(zx::msec(50))) {
    printf("Received unexpected callback!\n");
  }
}

void VirtualAudioUtil::QuitLoop() {
  async::PostTask(loop_->dispatcher(), [loop = loop_]() { loop->Quit(); });
}

// Below was borrowed from gtest, as-is
bool VirtualAudioUtil::RunLoopWithTimeout(zx::duration timeout) {
  auto canceled = std::make_shared<bool>(false);
  bool timed_out = false;
  async::PostDelayedTask(
      loop_->dispatcher(),
      [this, canceled, &timed_out] {
        if (*canceled) {
          return;
        }
        timed_out = true;
        loop_->Quit();
      },
      timeout);
  loop_->Run();
  loop_->ResetQuit();

  if (!timed_out) {
    *canceled = true;
  }
  return timed_out;
}
// Above was borrowed from gtest, as-is

bool VirtualAudioUtil::WaitForNoCallback() {
  bool timed_out = RunLoopWithTimeout(zx::msec(10));

  // If all is well, we DIDN'T get a disconnect callback and are still bound.
  return (timed_out);
}

bool VirtualAudioUtil::WaitForCallback() {
  bool timed_out = RunLoopWithTimeout(zx::msec(100));

  return !timed_out;
}

void VirtualAudioUtil::RegisterKeyWaiter() {
  keystroke_waiter_.Wait(
      [this](zx_status_t, uint32_t) {
        int c = std::tolower(getc(stdin));
        if (c == 'q') {
          key_quit_ = true;
        }
        QuitLoop();
      },
      STDIN_FILENO, POLLIN);
}

bool VirtualAudioUtil::WaitForKey() {
  printf("\tPress Q to cancel, or any other key to continue...\n");
  setbuf(stdin, nullptr);
  RegisterKeyWaiter();

  while (RunLoopWithTimeout(zx::sec(1))) {
  }

  return !key_quit_;
}

void VirtualAudioUtil::ConnectToController() {
  if (controller_.is_bound()) {
    return;
  }

  startup_context_->ConnectToEnvironmentService(controller_.NewRequest());
}

bool VirtualAudioUtil::ConnectToDevice() {
  return (configuring_output_ ? ConnectToOutput() : ConnectToInput());
}

bool VirtualAudioUtil::ConnectToInput() {
  if (input_.is_bound()) {
    return true;
  }

  startup_context_->ConnectToEnvironmentService(input_.NewRequest());

  input_.set_error_handler([this](zx_status_t error) {
    printf("input_ disconnected (%d)!\n", error);
    QuitLoop();
  });

  // let VirtualAudio disconnect if all is not well.
  bool success = (WaitForNoCallback() && input_.is_bound());

  if (!success) {
    printf("Failed to establish channel to input\n");
  }
  return success;
}

bool VirtualAudioUtil::ConnectToOutput() {
  if (output_.is_bound()) {
    return true;
  }

  startup_context_->ConnectToEnvironmentService(output_.NewRequest());

  output_.set_error_handler([this](zx_status_t error) {
    printf("output_ disconnected (%d)!\n", error);
    QuitLoop();
  });

  // let VirtualAudio disconnect if all is not well.
  bool success = (WaitForNoCallback() && output_.is_bound());

  if (!success) {
    printf("Failed to establish channel to output\n");
  }
  return success;
}

void VirtualAudioUtil::ParseAndExecute(fxl::CommandLine* cmdline) {
  if (!cmdline->has_argv0() || cmdline->options().size() == 0) {
    printf("No commands provided; no action taken\n");
    return;
  }

  // Looks like we will interact with the service; get ready to connect to it.
  startup_context_ = component::StartupContext::CreateFromStartupInfo();

  for (auto option : cmdline->options()) {
    bool success = false;
    Command cmd = Command::INVALID;

    for (const auto& entry : COMMANDS) {
      if (!option.name.compare(entry.name)) {
        cmd = entry.cmd;
        success = true;

        break;
      }
    }

    if (!success) {
      printf("Failed to parse command ID `--%s'\n", option.name.c_str());
      return;
    }

    printf("Executing `--%s' command...\n", option.name.c_str());
    success = ExecuteCommand(cmd, option.value);
    if (!success) {
      return;
    }
  }  // while (cmdline args) without default
}

bool VirtualAudioUtil::ExecuteCommand(Command cmd, const std::string& value) {
  bool success;
  switch (cmd) {
    // FIDL Service methods
    case Command::ENABLE_VIRTUAL_AUDIO:
      success = Enable(true);
      break;
    case Command::DISABLE_VIRTUAL_AUDIO:
      success = Enable(false);
      break;

    // FIDL Configuration/Device methods
    case Command::SET_DEVICE_NAME:
      success = SetDeviceName(value);
      break;
    case Command::SET_MANUFACTURER:
      success = SetManufacturer(value);
      break;
    case Command::SET_PRODUCT_NAME:
      success = SetProductName(value);
      break;
    case Command::SET_UNIQUE_ID:
      success = SetUniqueId(value);
      break;
    case Command::ADD_FORMAT_RANGE:
      success = AddFormatRange(value);
      break;
    case Command::SET_FIFO_DEPTH:
      success = SetFifoDepth(value);
      break;
    case Command::SET_EXTERNAL_DELAY:
      success = SetExternalDelay(value);
      break;
    case Command::SET_RING_BUFFER_RESTRICTIONS:
      success = SetRingBufferRestrictions(value);
      break;
    case Command::SET_GAIN_PROPS:
      success = SetGainProperties(value);
      break;
    case Command::SET_PLUG_PROPS:
      success = SetPlugProperties(value);
      break;
    case Command::RESET_CONFIG:
      success = ResetConfiguration();
      break;

    case Command::ADD_DEVICE:
      success = AddDevice();
      break;
    case Command::REMOVE_DEVICE:
      success = RemoveDevice();
      break;

    case Command::PLUG:
      success = ChangePlugState(value, true);
      break;
    case Command::UNPLUG:
      success = ChangePlugState(value, false);
      break;

    case Command::SET_IN:
      configuring_output_ = false;
      success = true;
      break;
    case Command::SET_OUT:
      configuring_output_ = true;
      success = true;
      break;
    case Command::WAIT:
      success = WaitForKey();
      break;
    case Command::INVALID:
      success = false;
      break;

      // Intentionally omitting default so new enums are not forgotten here.
  }
  return success;
}

bool VirtualAudioUtil::Enable(bool enable) {
  ConnectToController();

  zx_status_t status =
      (enable ? controller_->Enable() : controller_->Disable());
  if (status != ZX_OK) {
    printf("ControlSync::%sable failed (%d)!\n", (enable ? "En" : "Dis"),
           status);
    return false;
  }
  return true;
}

bool VirtualAudioUtil::SetDeviceName(const std::string& name) {
  if (!ConnectToDevice()) {
    return false;
  }

  if (configuring_output_) {
    output_->SetDeviceName(name == "" ? kDefaultDeviceName : name);
  } else {
    input_->SetDeviceName(name == "" ? kDefaultDeviceName : name);
  }
  return WaitForNoCallback();
}

bool VirtualAudioUtil::SetManufacturer(const std::string& name) {
  if (!ConnectToDevice()) {
    return false;
  }

  if (configuring_output_) {
    output_->SetManufacturer(name == "" ? kDefaultManufacturer : name);
  } else {
    input_->SetManufacturer(name == "" ? kDefaultManufacturer : name);
  }
  return WaitForNoCallback();
}

bool VirtualAudioUtil::SetProductName(const std::string& name) {
  if (!ConnectToDevice()) {
    return false;
  }

  if (configuring_output_) {
    output_->SetProduct(name == "" ? kDefaultProductName : name);
  } else {
    input_->SetProduct(name == "" ? kDefaultProductName : name);
  }
  return WaitForNoCallback();
}

bool VirtualAudioUtil::SetUniqueId(const std::string& unique_id_str) {
  if (!ConnectToDevice()) {
    return false;
  }

  fidl::Array<uint8_t, 16> unique_id;
  bool use_default = (unique_id_str == "");

  for (uint8_t index = 0; index < 16; ++index) {
    unique_id[index] =
        use_default
            ? kDefaultUniqueId[index]
            : unique_id_str.size() <= (2 * index + 1)
                  ? 0
                  : fxl::StringToNumber<uint8_t>(
                        unique_id_str.substr(index * 2, 2), fxl::Base::k16);
  }
  if (configuring_output_) {
    output_->SetUniqueId(unique_id);
  } else {
    input_->SetUniqueId(unique_id);
  }
  return WaitForNoCallback();
}

struct Format {
  uint32_t flags;
  uint32_t min_rate;
  uint32_t max_rate;
  uint32_t min_chans;
  uint32_t max_chans;
  uint32_t rate_family_flags;
};

constexpr Format kFormatSpecs[4] = {
    {.flags = AUDIO_SAMPLE_FORMAT_16BIT | AUDIO_SAMPLE_FORMAT_24BIT_IN32,
     .min_rate = 8000,
     .max_rate = 44100,
     .min_chans = 1,
     .max_chans = 2,
     .rate_family_flags =
         ASF_RANGE_FLAG_FPS_44100_FAMILY | ASF_RANGE_FLAG_FPS_48000_FAMILY},
    {.flags = AUDIO_SAMPLE_FORMAT_32BIT_FLOAT,
     .min_rate = 32000,
     .max_rate = 96000,
     .min_chans = 2,
     .max_chans = 4,
     .rate_family_flags = ASF_RANGE_FLAG_FPS_48000_FAMILY},
    {.flags = AUDIO_SAMPLE_FORMAT_16BIT,
     .min_rate = 48000,
     .max_rate = 48000,
     .min_chans = 2,
     .max_chans = 2,
     .rate_family_flags = ASF_RANGE_FLAG_FPS_48000_FAMILY},
    {.flags = AUDIO_SAMPLE_FORMAT_16BIT,
     .min_rate = 16000,
     .max_rate = 16000,
     .min_chans = 2,
     .max_chans = 2,
     .rate_family_flags = ASF_RANGE_FLAG_FPS_48000_FAMILY}};

bool VirtualAudioUtil::AddFormatRange(const std::string& format_range_str) {
  if (!ConnectToDevice()) {
    return false;
  }

  uint8_t format_option =
      (format_range_str == "" ? kDefaultFormatRangeOption
                              : fxl::StringToNumber<uint8_t>(format_range_str));
  if (format_option >= fbl::count_of(kFormatSpecs)) {
    printf("Format range option must be %lu or less.\n",
           fbl::count_of(kFormatSpecs) - 1);
    return false;
  }

  if (configuring_output_) {
    output_->AddFormatRange(kFormatSpecs[format_option].flags,
                            kFormatSpecs[format_option].min_rate,
                            kFormatSpecs[format_option].max_rate,
                            kFormatSpecs[format_option].min_chans,
                            kFormatSpecs[format_option].max_chans,
                            kFormatSpecs[format_option].rate_family_flags);
  } else {
    input_->AddFormatRange(kFormatSpecs[format_option].flags,
                           kFormatSpecs[format_option].min_rate,
                           kFormatSpecs[format_option].max_rate,
                           kFormatSpecs[format_option].min_chans,
                           kFormatSpecs[format_option].max_chans,
                           kFormatSpecs[format_option].rate_family_flags);
  }
  return WaitForNoCallback();
}

bool VirtualAudioUtil::SetFifoDepth(const std::string& fifo_str) {
  if (!ConnectToDevice()) {
    return false;
  }

  uint32_t fifo_depth =
      (fifo_str == "" ? kDefaultFifoDepth
                      : fxl::StringToNumber<uint32_t>(fifo_str));
  if (configuring_output_) {
    output_->SetFifoDepth(fifo_depth);
  } else {
    input_->SetFifoDepth(fifo_depth);
  }

  return WaitForNoCallback();
}

bool VirtualAudioUtil::SetExternalDelay(const std::string& delay_str) {
  if (!ConnectToDevice()) {
    return false;
  }

  zx_duration_t external_delay =
      (delay_str == "" ? kDefaultExternalDelayNsec
                       : fxl::StringToNumber<zx_duration_t>(delay_str));
  if (configuring_output_) {
    output_->SetExternalDelay(external_delay);
  } else {
    input_->SetExternalDelay(external_delay);
  }

  return WaitForNoCallback();
}

struct BufferSpec {
  uint32_t min_frames;
  uint32_t max_frames;
  uint32_t mod_frames;
};

constexpr BufferSpec kBufferSpecs[2] = {
    {.min_frames = 50000, .max_frames = 70000, .mod_frames = 10000},
    {.min_frames = 40000, .max_frames = 50000, .mod_frames = 1000}};

bool VirtualAudioUtil::SetRingBufferRestrictions(
    const std::string& rb_restr_str) {
  if (!ConnectToDevice()) {
    return false;
  }

  uint8_t rb_option =
      (rb_restr_str == "" ? kDefaultRingBufferOption
                          : fxl::StringToNumber<uint8_t>(rb_restr_str));
  if (rb_option >= fbl::count_of(kBufferSpecs)) {
    printf("Ring buffer option must be %lu or less. Using value of %u.\n",
           fbl::count_of(kBufferSpecs) - 1, rb_option);
    return false;
  }

  if (configuring_output_) {
    output_->SetRingBufferRestrictions(kBufferSpecs[rb_option].min_frames,
                                       kBufferSpecs[rb_option].max_frames,
                                       kBufferSpecs[rb_option].mod_frames);
  } else {
    input_->SetRingBufferRestrictions(kBufferSpecs[rb_option].min_frames,
                                      kBufferSpecs[rb_option].max_frames,
                                      kBufferSpecs[rb_option].mod_frames);
  }

  return WaitForNoCallback();
}

struct GainSpec {
  bool cur_mute;
  bool cur_agc;
  float cur_gain_db;
  bool can_mute;
  bool can_agc;
  float min_gain_db;
  float max_gain_db;
  float gain_step_db;
};

// The utility defines two preset groups of gain options. Although arbitrarily
// chosen, they exercise the available range through SetGainProperties:
// 0.Can and is mute.    Cannot AGC.       Gain -1, range [-60, 0] in 2.0dB.
// 1.Can but isn't mute. Can AGC, enabled. Gain -12,range [-30,+2] in 0.5db.
// 2 and above represent invalid combinations.
constexpr GainSpec kGainSpecs[4] = {{.cur_mute = true,
                                     .cur_agc = false,
                                     .cur_gain_db = -1.0,
                                     .can_mute = true,
                                     .can_agc = false,
                                     .min_gain_db = -60.0,
                                     .max_gain_db = 0.0,
                                     .gain_step_db = 2.0},
                                    {.cur_mute = false,
                                     .cur_agc = true,
                                     .cur_gain_db = -12.0,
                                     .can_mute = true,
                                     .can_agc = true,
                                     .min_gain_db = -30.0,
                                     .max_gain_db = 2.0,
                                     .gain_step_db = 0.5},
                                    {.cur_mute = true,
                                     .cur_agc = true,
                                     .cur_gain_db = -12.0,
                                     .can_mute = false,
                                     .can_agc = false,
                                     .min_gain_db = -96.0,
                                     .max_gain_db = 0.0,
                                     .gain_step_db = 1.0},
                                    {.cur_mute = false,
                                     .cur_agc = false,
                                     .cur_gain_db = 50.0,
                                     .can_mute = true,
                                     .can_agc = false,
                                     .min_gain_db = 20.0,
                                     .max_gain_db = -20.0,
                                     .gain_step_db = -3.0}};

bool VirtualAudioUtil::SetGainProperties(const std::string& gain_props_str) {
  if (!ConnectToDevice()) {
    return false;
  }

  uint8_t gain_props_option =
      (gain_props_str == "" ? kDefaultGainPropsOption
                            : fxl::StringToNumber<uint8_t>(gain_props_str));
  if (gain_props_option >= fbl::count_of(kGainSpecs)) {
    printf("Gain properties option must be %lu or less.\n",
           fbl::count_of(kGainSpecs));
    return false;
  }

  if (configuring_output_) {
    output_->SetGainProperties(kGainSpecs[gain_props_option].min_gain_db,
                               kGainSpecs[gain_props_option].max_gain_db,
                               kGainSpecs[gain_props_option].gain_step_db,
                               kGainSpecs[gain_props_option].cur_gain_db,
                               kGainSpecs[gain_props_option].can_mute,
                               kGainSpecs[gain_props_option].cur_mute,
                               kGainSpecs[gain_props_option].can_agc,
                               kGainSpecs[gain_props_option].cur_agc);
  } else {
    input_->SetGainProperties(kGainSpecs[gain_props_option].min_gain_db,
                              kGainSpecs[gain_props_option].max_gain_db,
                              kGainSpecs[gain_props_option].gain_step_db,
                              kGainSpecs[gain_props_option].cur_gain_db,
                              kGainSpecs[gain_props_option].can_mute,
                              kGainSpecs[gain_props_option].cur_mute,
                              kGainSpecs[gain_props_option].can_agc,
                              kGainSpecs[gain_props_option].cur_agc);
  }

  return WaitForNoCallback();
}

// These preset options represent the following common configurations:
// 0.(Default) Hot-pluggable;   1.Hardwired;    2.Hot-pluggable, unplugged;
// 3.Plugged (synch: detected only by polling); 4.Unplugged (synch)
constexpr audio_pd_notify_flags_t kPlugFlags[] = {
    AUDIO_PDNF_PLUGGED /*AUDIO_PDNF_HARDWIRED*/ | AUDIO_PDNF_CAN_NOTIFY,
    AUDIO_PDNF_PLUGGED | AUDIO_PDNF_HARDWIRED /*  AUDIO_PDNF_CAN_NOTIFY*/,
    /*AUDIO_PDNF_PLUGGED AUDIO_PDNF_HARDWIRED  */ AUDIO_PDNF_CAN_NOTIFY,
    AUDIO_PDNF_PLUGGED /*AUDIO_PDNF_HARDWIRED     AUDIO_PDNF_CAN_NOTIFY*/,
    0 /*AUDIO_PDNF_PLUGGED AUDIO_PDNF_HARDWIRED   AUDIO_PDNF_CAN_NOTIFY*/
};

constexpr zx_time_t kPlugTime[] = {0, -1, -1, ZX_SEC(1), ZX_SEC(2)};
static_assert(fbl::count_of(kPlugFlags) == fbl::count_of(kPlugTime));

bool VirtualAudioUtil::SetPlugProperties(const std::string& plug_props_str) {
  if (!ConnectToDevice()) {
    return false;
  }

  uint8_t plug_props_option =
      (plug_props_str == "" ? kDefaultPlugPropsOption
                            : fxl::StringToNumber<uint8_t>(plug_props_str));

  if (plug_props_option >= fbl::count_of(kPlugFlags)) {
    printf("Plug properties option must be %lu or less. Using value of %u.\n",
           fbl::count_of(kPlugFlags) - 1, plug_props_option);
    return false;
  }

  zx_time_t plug_change_time =
      (kPlugTime[plug_props_option] == -1 ? zx_clock_get(ZX_CLOCK_MONOTONIC)
                                          : kPlugTime[plug_props_option]);
  bool plugged = (kPlugFlags[plug_props_option] & AUDIO_PDNF_PLUGGED);
  bool hardwired = (kPlugFlags[plug_props_option] & AUDIO_PDNF_HARDWIRED);
  bool can_notify = (kPlugFlags[plug_props_option] & AUDIO_PDNF_CAN_NOTIFY);

  if (configuring_output_) {
    output_->SetPlugProperties(plug_change_time, plugged, hardwired,
                               can_notify);
  } else {
    input_->SetPlugProperties(plug_change_time, plugged, hardwired, can_notify);
  }

  return WaitForNoCallback();
}

bool VirtualAudioUtil::ResetConfiguration() {
  if (!ConnectToDevice()) {
    return false;
  }

  if (configuring_output_) {
    output_->ResetConfiguration();
  } else {
    input_->ResetConfiguration();
  }

  return WaitForNoCallback();
}

bool VirtualAudioUtil::AddDevice() {
  if (!ConnectToDevice()) {
    return false;
  }

  if (configuring_output_) {
    output_->Add();
  } else {
    input_->Add();
  }

  return WaitForNoCallback();
}

bool VirtualAudioUtil::RemoveDevice() {
  if (!ConnectToDevice()) {
    return false;
  }

  if (configuring_output_) {
    output_->Remove();
  } else {
    input_->Remove();
  }

  return WaitForNoCallback();
}

bool VirtualAudioUtil::ChangePlugState(const std::string& plug_time_str,
                                       bool plugged) {
  if (!ConnectToDevice()) {
    return false;
  }

  zx_time_t plug_change_time =
      (plug_time_str == "" ? zx_clock_get(ZX_CLOCK_MONOTONIC)
                           : fxl::StringToNumber<zx_time_t>(plug_time_str));

  if (configuring_output_) {
    output_->ChangePlugState(plug_change_time, plugged);
  } else {
    input_->ChangePlugState(plug_change_time, plugged);
  }

  return WaitForNoCallback();
}

}  // namespace virtual_audio

int main(int argc, const char** argv) {
  fxl::CommandLine command_line = fxl::CommandLineFromArgcArgv(argc, argv);
  async::Loop loop(&kAsyncLoopConfigAttachToThread);

  ::virtual_audio::VirtualAudioUtil util(&loop);
  util.Run(&command_line);

  return 0;
}
