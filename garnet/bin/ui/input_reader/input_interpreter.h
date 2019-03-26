// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_BIN_UI_INPUT_READER_INPUT_INTERPRETER_H_
#define GARNET_BIN_UI_INPUT_READER_INPUT_INTERPRETER_H_

#include <hid/acer12.h>
#include <lib/zx/event.h>
#include <zircon/types.h>

#include <array>
#include <string>
#include <vector>

#include "garnet/bin/ui/input_reader/buttons.h"
#include "garnet/bin/ui/input_reader/hardcoded.h"
#include "garnet/bin/ui/input_reader/hid_decoder.h"
#include "garnet/bin/ui/input_reader/protocols.h"
#include "garnet/bin/ui/input_reader/sensor.h"
#include "garnet/bin/ui/input_reader/touchpad.h"
#include "garnet/bin/ui/input_reader/touchscreen.h"

#include <fuchsia/ui/input/cpp/fidl.h>

namespace mozart {

// Each InputInterpreter instance observes and routes events coming in from one
// file descriptor under /dev/class/input. Each file descriptor may multiplex
// events from one or more physical devices, though typically there is a 1:1
// correspondence for input devices like keyboards and mice. Sensors are an
// atypical case, where many sensors have their events routed through one
// logical file descriptor, since they share a hardware FIFO queue.

class InputInterpreter {
 public:
  enum ReportType {
    kKeyboard,
    kMouse,
    kStylus,
    kTouchscreen,
  };

  static std::unique_ptr<InputInterpreter> Open(
      int dirfd, std::string filename,
      fuchsia::ui::input::InputDeviceRegistry* registry);

  InputInterpreter(std::unique_ptr<HidDecoder> hid_decoder,
                   fuchsia::ui::input::InputDeviceRegistry* registry);
  ~InputInterpreter();

  bool Initialize();
  bool Read(bool discard);

  const std::string& name() const { return hid_decoder_->name(); }
  zx_handle_t handle() { return event_.get(); }

 private:
  // Each InputDevice represents a logical device exposed by a HID device.
  // Some HID devices have multiple InputDevices (e.g: A keyboard/mouse
  // combo with a single USB cable).
  struct InputDevice {
    // The Device struct that parses the reports.
    std::unique_ptr<Device> device;
    // The structured report that is parsed by |device|.
    fuchsia::ui::input::InputReportPtr report;
    // Holds descriptions of what this device contains.
    Device::Descriptor descriptor;
    // The pointer where reports are sent to by this device.
    fuchsia::ui::input::InputDevicePtr input_device;
  };

  // Helper function called during Init() that determines which protocol
  // is going to be used. If it returns true then |protocol_| has been
  // set correctly.
  bool ParseProtocol();

  void NotifyRegistry();

  fuchsia::ui::input::InputDeviceRegistry* registry_;

  zx::event event_;

  // The array of Devices that are managed by this InputInterpreter.
  std::vector<InputDevice> devices_;

  std::unique_ptr<HidDecoder> hid_decoder_;

  // TODO(SCN-1251) All of the below variables are only used with devices that
  // have not been updated to the new code path. They will hopefully be removed
  // before long.

  static const uint8_t kMaxSensorCount = 16;
  static const uint8_t kNoSuchSensor = 0xFF;

  bool has_keyboard_ = false;
  fuchsia::ui::input::KeyboardDescriptorPtr keyboard_descriptor_;
  bool has_buttons_ = false;
  fuchsia::ui::input::ButtonsDescriptorPtr buttons_descriptor_;
  bool has_mouse_ = false;
  fuchsia::ui::input::MouseDescriptorPtr mouse_descriptor_;
  bool has_stylus_ = false;
  fuchsia::ui::input::StylusDescriptorPtr stylus_descriptor_;
  bool has_touchscreen_ = false;
  fuchsia::ui::input::TouchscreenDescriptorPtr touchscreen_descriptor_;
  bool has_sensors_ = false;
  // Arrays are indexed by the sensor number that was assigned by Zircon.
  // Keeps track of the physical sensors multiplexed over the file descriptor.
  std::array<fuchsia::ui::input::SensorDescriptorPtr, kMaxSensorCount>
      sensor_descriptors_;
  std::array<fuchsia::ui::input::InputDevicePtr, kMaxSensorCount>
      sensor_devices_;

  TouchDeviceType touch_device_type_ = TouchDeviceType::NONE;
  MouseDeviceType mouse_device_type_ = MouseDeviceType::NONE;
  SensorDeviceType sensor_device_type_ = SensorDeviceType::NONE;

  // Keep track of which sensor gave us a report. Index into
  // |sensor_descriptors_| and |sensor_devices_|.
  uint8_t sensor_idx_ = kNoSuchSensor;

  fuchsia::ui::input::InputReportPtr keyboard_report_;
  fuchsia::ui::input::InputReportPtr mouse_report_;
  fuchsia::ui::input::InputReportPtr touchscreen_report_;
  fuchsia::ui::input::InputReportPtr stylus_report_;
  fuchsia::ui::input::InputReportPtr sensor_report_;
  fuchsia::ui::input::InputReportPtr buttons_report_;

  fuchsia::ui::input::InputDevicePtr input_device_;

  Protocol protocol_;
  Hardcoded hardcoded_ = {};
};

}  // namespace mozart

#endif  // GARNET_BIN_UI_INPUT_READER_INPUT_INTERPRETER_H_
