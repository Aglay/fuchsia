// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/ui/input_reader/input_interpreter.h"
#include "garnet/bin/ui/input_reader/device.h"

#include <fuchsia/hardware/input/c/fidl.h>
#include <fuchsia/ui/input/cpp/fidl.h>
#include <hid-parser/parser.h>
#include <hid-parser/usages.h>
#include <hid/acer12.h>
#include <hid/ambient-light.h>
#include <hid/boot.h>
#include <hid/egalax.h>
#include <hid/eyoyo.h>
#include <hid/ft3x27.h>
#include <hid/hid.h>
#include <hid/paradise.h>
#include <hid/samsung.h>
#include <hid/usages.h>
#include <lib/fidl/cpp/clone.h>
#include <lib/fxl/arraysize.h>
#include <lib/fxl/logging.h>
#include <lib/fxl/time/time_point.h>
#include <lib/ui/input/cpp/formatting.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <trace/event.h>
#include <zircon/errors.h>
#include <zircon/types.h>

#include "garnet/bin/ui/input_reader/fdio_hid_decoder.h"
#include "garnet/bin/ui/input_reader/protocols.h"

namespace {

// Variable to quickly re-enable the hardcoded touchpad reports.
// TODO(ZX-3219): Remove this once touchpads are stable
bool USE_TOUCHPAD_HARDCODED_REPORTS = false;

int64_t InputEventTimestampNow() {
  return fxl::TimePoint::Now().ToEpochDelta().ToNanoseconds();
}

fuchsia::ui::input::InputReport CloneReport(
    const fuchsia::ui::input::InputReport& report) {
  fuchsia::ui::input::InputReport result;
  fidl::Clone(report, &result);
  return result;
}

// TODO(SCN-473): Extract sensor IDs from HID.
const size_t kParadiseAccLid = 0;
const size_t kParadiseAccBase = 1;
const size_t kAmbientLight = 2;
}  // namespace

namespace mozart {

InputInterpreter::InputInterpreter(
    std::unique_ptr<HidDecoder> hid_decoder,
    fuchsia::ui::input::InputDeviceRegistry* registry)
    : registry_(registry), hid_decoder_(std::move(hid_decoder)) {
  FXL_DCHECK(hid_decoder_);
}

InputInterpreter::~InputInterpreter() {}

bool InputInterpreter::Initialize() {
  if (!hid_decoder_->Init())
    return false;

  if (!ParseProtocol())
    return false;

  auto protocol = protocol_;

  if (protocol == Protocol::Keyboard) {
    FXL_VLOG(2) << "Device " << name() << " has keyboard";
    has_keyboard_ = true;
    keyboard_descriptor_ = fuchsia::ui::input::KeyboardDescriptor::New();
    keyboard_descriptor_->keys.resize(HID_USAGE_KEY_RIGHT_GUI -
                                      HID_USAGE_KEY_A + 1);
    for (size_t index = HID_USAGE_KEY_A; index <= HID_USAGE_KEY_RIGHT_GUI;
         ++index) {
      keyboard_descriptor_->keys.at(index - HID_USAGE_KEY_A) = index;
    }

    keyboard_report_ = fuchsia::ui::input::InputReport::New();
    keyboard_report_->keyboard = fuchsia::ui::input::KeyboardReport::New();
  } else if (protocol == Protocol::Buttons) {
    FXL_VLOG(2) << "Device " << name() << " has buttons";
    has_buttons_ = true;
    buttons_descriptor_ = fuchsia::ui::input::ButtonsDescriptor::New();
    buttons_descriptor_->buttons |= fuchsia::ui::input::kVolumeUp;
    buttons_descriptor_->buttons |= fuchsia::ui::input::kVolumeDown;
    buttons_descriptor_->buttons |= fuchsia::ui::input::kMicMute;
    buttons_report_ = fuchsia::ui::input::InputReport::New();
    buttons_report_->buttons = fuchsia::ui::input::ButtonsReport::New();
  } else if (protocol == Protocol::Mouse) {
    FXL_VLOG(2) << "Device " << name() << " has mouse";
    mouse_report_ = fuchsia::ui::input::InputReport::New();
    mouse_report_->mouse = fuchsia::ui::input::MouseReport::New();
  } else if (protocol == Protocol::BootMouse || protocol == Protocol::Gamepad) {
    FXL_VLOG(2) << "Device " << name() << " has mouse";
    has_mouse_ = true;
    mouse_device_type_ = (protocol == Protocol::BootMouse)
                             ? MouseDeviceType::BOOT
                             : MouseDeviceType::GAMEPAD;

    mouse_descriptor_ = fuchsia::ui::input::MouseDescriptor::New();
    mouse_descriptor_->rel_x.range.min = INT32_MIN;
    mouse_descriptor_->rel_x.range.max = INT32_MAX;
    mouse_descriptor_->rel_x.resolution = 1;

    mouse_descriptor_->rel_y.range.min = INT32_MIN;
    mouse_descriptor_->rel_y.range.max = INT32_MAX;
    mouse_descriptor_->rel_y.resolution = 1;

    mouse_descriptor_->buttons |= fuchsia::ui::input::kMouseButtonPrimary;
    mouse_descriptor_->buttons |= fuchsia::ui::input::kMouseButtonSecondary;
    mouse_descriptor_->buttons |= fuchsia::ui::input::kMouseButtonTertiary;

    mouse_report_ = fuchsia::ui::input::InputReport::New();
    mouse_report_->mouse = fuchsia::ui::input::MouseReport::New();
  } else if (protocol == Protocol::Touch) {
    FXL_VLOG(2) << "Device " << name() << " has hid touch";

    has_touchscreen_ = true;
    touchscreen_descriptor_ = fuchsia::ui::input::TouchscreenDescriptor::New();
    Touchscreen::Descriptor touch_desc;
    SetDescriptor(&touch_desc);
    touchscreen_descriptor_->x.range.min = touch_desc.x_min;
    touchscreen_descriptor_->x.range.max = touch_desc.x_max;
    touchscreen_descriptor_->x.resolution = touch_desc.x_resolution;

    touchscreen_descriptor_->y.range.min = touch_desc.y_min;
    touchscreen_descriptor_->y.range.max = touch_desc.y_max;
    touchscreen_descriptor_->y.resolution = touch_desc.x_resolution;

    touchscreen_descriptor_->max_finger_id = touch_desc.max_finger_id;

    touchscreen_report_ = fuchsia::ui::input::InputReport::New();
    touchscreen_report_->touchscreen =
        fuchsia::ui::input::TouchscreenReport::New();

    touch_device_type_ = TouchDeviceType::HID;
  } else if (protocol == Protocol::Touchpad) {
    FXL_VLOG(2) << "Device " << name() << " has hid touchpad";

    has_mouse_ = true;
    mouse_descriptor_ = fuchsia::ui::input::MouseDescriptor::New();
    mouse_device_type_ = MouseDeviceType::TOUCH;

    mouse_descriptor_->rel_x.range.min = INT32_MIN;
    mouse_descriptor_->rel_x.range.max = INT32_MAX;
    mouse_descriptor_->rel_x.resolution = 1;

    mouse_descriptor_->rel_y.range.min = INT32_MIN;
    mouse_descriptor_->rel_y.range.max = INT32_MAX;
    mouse_descriptor_->rel_y.resolution = 1;

    mouse_descriptor_->buttons |= fuchsia::ui::input::kMouseButtonPrimary;

    mouse_report_ = fuchsia::ui::input::InputReport::New();
    mouse_report_->mouse = fuchsia::ui::input::MouseReport::New();
  } else if (protocol == Protocol::Acer12Touch) {
    FXL_VLOG(2) << "Device " << name() << " has stylus";
    has_stylus_ = true;
    stylus_descriptor_ = fuchsia::ui::input::StylusDescriptor::New();

    stylus_descriptor_->x.range.min = 0;
    stylus_descriptor_->x.range.max = ACER12_STYLUS_X_MAX;
    stylus_descriptor_->x.resolution = 1;

    stylus_descriptor_->y.range.min = 0;
    stylus_descriptor_->y.range.max = ACER12_STYLUS_Y_MAX;
    stylus_descriptor_->y.resolution = 1;

    stylus_descriptor_->is_invertible = false;

    stylus_descriptor_->buttons |= fuchsia::ui::input::kStylusBarrel;

    stylus_report_ = fuchsia::ui::input::InputReport::New();
    stylus_report_->stylus = fuchsia::ui::input::StylusReport::New();

    FXL_VLOG(2) << "Device " << name() << " has touchscreen";
    has_touchscreen_ = true;
    touchscreen_descriptor_ = fuchsia::ui::input::TouchscreenDescriptor::New();

    touchscreen_descriptor_->x.range.min = 0;
    touchscreen_descriptor_->x.range.max = ACER12_X_MAX;
    touchscreen_descriptor_->x.resolution = 1;

    touchscreen_descriptor_->y.range.min = 0;
    touchscreen_descriptor_->y.range.max = ACER12_Y_MAX;
    touchscreen_descriptor_->y.resolution = 1;

    // TODO(jpoichet) do not hardcode this
    touchscreen_descriptor_->max_finger_id = 255;

    touchscreen_report_ = fuchsia::ui::input::InputReport::New();
    touchscreen_report_->touchscreen =
        fuchsia::ui::input::TouchscreenReport::New();

    touch_device_type_ = TouchDeviceType::ACER12;
  } else if (protocol == Protocol::SamsungTouch) {
    FXL_VLOG(2) << "Device " << name() << " has touchscreen";
    has_touchscreen_ = true;
    touchscreen_descriptor_ = fuchsia::ui::input::TouchscreenDescriptor::New();

    touchscreen_descriptor_->x.range.min = 0;
    touchscreen_descriptor_->x.range.max = SAMSUNG_X_MAX;
    touchscreen_descriptor_->x.resolution = 1;

    touchscreen_descriptor_->y.range.min = 0;
    touchscreen_descriptor_->y.range.max = SAMSUNG_Y_MAX;
    touchscreen_descriptor_->y.resolution = 1;

    // TODO(jpoichet) do not hardcode this
    touchscreen_descriptor_->max_finger_id = 255;

    touchscreen_report_ = fuchsia::ui::input::InputReport::New();
    touchscreen_report_->touchscreen =
        fuchsia::ui::input::TouchscreenReport::New();

    touch_device_type_ = TouchDeviceType::SAMSUNG;
  } else if (protocol == Protocol::ParadiseV1Touch) {
    // TODO(cpu): Add support for stylus.
    FXL_VLOG(2) << "Device " << name() << " has touchscreen";
    has_touchscreen_ = true;
    touchscreen_descriptor_ = fuchsia::ui::input::TouchscreenDescriptor::New();

    touchscreen_descriptor_->x.range.min = 0;
    touchscreen_descriptor_->x.range.max = PARADISE_X_MAX;
    touchscreen_descriptor_->x.resolution = 1;

    touchscreen_descriptor_->y.range.min = 0;
    touchscreen_descriptor_->y.range.max = PARADISE_Y_MAX;
    touchscreen_descriptor_->y.resolution = 1;

    // TODO(cpu) do not hardcode |max_finger_id|.
    touchscreen_descriptor_->max_finger_id = 255;

    touchscreen_report_ = fuchsia::ui::input::InputReport::New();
    touchscreen_report_->touchscreen =
        fuchsia::ui::input::TouchscreenReport::New();

    touch_device_type_ = TouchDeviceType::PARADISEv1;
  } else if (protocol == Protocol::ParadiseV2Touch) {
    FXL_VLOG(2) << "Device " << name() << " has stylus";
    has_stylus_ = true;
    stylus_descriptor_ = fuchsia::ui::input::StylusDescriptor::New();

    stylus_descriptor_->x.range.min = 0;
    stylus_descriptor_->x.range.max = PARADISE_STYLUS_X_MAX;
    stylus_descriptor_->x.resolution = 1;

    stylus_descriptor_->y.range.min = 0;
    stylus_descriptor_->y.range.max = PARADISE_STYLUS_Y_MAX;
    stylus_descriptor_->y.resolution = 1;

    stylus_descriptor_->is_invertible = false;

    stylus_descriptor_->buttons |= fuchsia::ui::input::kStylusBarrel;

    stylus_report_ = fuchsia::ui::input::InputReport::New();
    stylus_report_->stylus = fuchsia::ui::input::StylusReport::New();

    FXL_VLOG(2) << "Device " << name() << " has touchscreen";
    has_touchscreen_ = true;
    touchscreen_descriptor_ = fuchsia::ui::input::TouchscreenDescriptor::New();

    touchscreen_descriptor_->x.range.min = 0;
    touchscreen_descriptor_->x.range.max = PARADISE_X_MAX;
    touchscreen_descriptor_->x.resolution = 1;

    touchscreen_descriptor_->y.range.min = 0;
    touchscreen_descriptor_->y.range.max = PARADISE_Y_MAX;
    touchscreen_descriptor_->y.resolution = 1;

    // TODO(cpu) do not hardcode |max_finger_id|.
    touchscreen_descriptor_->max_finger_id = 255;

    touchscreen_report_ = fuchsia::ui::input::InputReport::New();
    touchscreen_report_->touchscreen =
        fuchsia::ui::input::TouchscreenReport::New();

    touch_device_type_ = TouchDeviceType::PARADISEv2;
  } else if (protocol == Protocol::ParadiseV3Touch) {
    FXL_VLOG(2) << "Device " << name() << " has stylus";
    has_stylus_ = true;
    stylus_descriptor_ = fuchsia::ui::input::StylusDescriptor::New();

    stylus_descriptor_->x.range.min = 0;
    stylus_descriptor_->x.range.max = PARADISE_STYLUS_X_MAX;
    stylus_descriptor_->x.resolution = 1;

    stylus_descriptor_->y.range.min = 0;
    stylus_descriptor_->y.range.max = PARADISE_STYLUS_Y_MAX;
    stylus_descriptor_->y.resolution = 1;

    stylus_descriptor_->is_invertible = false;

    stylus_descriptor_->buttons |= fuchsia::ui::input::kStylusBarrel;

    stylus_report_ = fuchsia::ui::input::InputReport::New();
    stylus_report_->stylus = fuchsia::ui::input::StylusReport::New();

    FXL_VLOG(2) << "Device " << name() << " has touchscreen";
    has_touchscreen_ = true;
    touchscreen_descriptor_ = fuchsia::ui::input::TouchscreenDescriptor::New();

    touchscreen_descriptor_->x.range.min = 0;
    touchscreen_descriptor_->x.range.max = PARADISE_X_MAX;
    touchscreen_descriptor_->x.resolution = 1;

    touchscreen_descriptor_->y.range.min = 0;
    touchscreen_descriptor_->y.range.max = PARADISE_Y_MAX;
    touchscreen_descriptor_->y.resolution = 1;

    // TODO(cpu) do not hardcode |max_finger_id|.
    touchscreen_descriptor_->max_finger_id = 255;

    touchscreen_report_ = fuchsia::ui::input::InputReport::New();
    touchscreen_report_->touchscreen =
        fuchsia::ui::input::TouchscreenReport::New();

    touch_device_type_ = TouchDeviceType::PARADISEv3;
  } else if (protocol == Protocol::ParadiseV1TouchPad) {
    FXL_VLOG(2) << "Device " << name() << " has touchpad";
    has_mouse_ = true;
    mouse_device_type_ = MouseDeviceType::PARADISEv1;

    mouse_descriptor_ = fuchsia::ui::input::MouseDescriptor::New();

    mouse_descriptor_->rel_x.range.min = INT32_MIN;
    mouse_descriptor_->rel_x.range.max = INT32_MAX;
    mouse_descriptor_->rel_x.resolution = 1;

    mouse_descriptor_->rel_y.range.min = INT32_MIN;
    mouse_descriptor_->rel_y.range.max = INT32_MAX;
    mouse_descriptor_->rel_y.resolution = 1;

    mouse_descriptor_->buttons |= fuchsia::ui::input::kMouseButtonPrimary;

    mouse_report_ = fuchsia::ui::input::InputReport::New();
    mouse_report_->mouse = fuchsia::ui::input::MouseReport::New();
  } else if (protocol == Protocol::ParadiseV2TouchPad) {
    FXL_VLOG(2) << "Device " << name() << " has touchpad";
    has_mouse_ = true;
    mouse_device_type_ = MouseDeviceType::PARADISEv2;

    mouse_descriptor_ = fuchsia::ui::input::MouseDescriptor::New();

    mouse_descriptor_->rel_x.range.min = INT32_MIN;
    mouse_descriptor_->rel_x.range.max = INT32_MAX;
    mouse_descriptor_->rel_x.resolution = 1;

    mouse_descriptor_->rel_y.range.min = INT32_MIN;
    mouse_descriptor_->rel_y.range.max = INT32_MAX;
    mouse_descriptor_->rel_y.resolution = 1;

    mouse_descriptor_->buttons |= fuchsia::ui::input::kMouseButtonPrimary;

    mouse_report_ = fuchsia::ui::input::InputReport::New();
    mouse_report_->mouse = fuchsia::ui::input::MouseReport::New();
  } else if (protocol == Protocol::EgalaxTouch) {
    FXL_VLOG(2) << "Device " << name() << " has touchscreen";
    has_touchscreen_ = true;
    touchscreen_descriptor_ = fuchsia::ui::input::TouchscreenDescriptor::New();

    touchscreen_descriptor_->x.range.min = 0;
    touchscreen_descriptor_->x.range.max = EGALAX_X_MAX;
    touchscreen_descriptor_->x.resolution = 1;

    touchscreen_descriptor_->y.range.min = 0;
    touchscreen_descriptor_->y.range.max = EGALAX_Y_MAX;
    touchscreen_descriptor_->y.resolution = 1;

    touchscreen_descriptor_->max_finger_id = 1;

    touchscreen_report_ = fuchsia::ui::input::InputReport::New();
    touchscreen_report_->touchscreen =
        fuchsia::ui::input::TouchscreenReport::New();

    touch_device_type_ = TouchDeviceType::EGALAX;
  } else if (protocol == Protocol::ParadiseSensor) {
    FXL_VLOG(2) << "Device " << name() << " has motion sensors";
    sensor_device_type_ = SensorDeviceType::PARADISE;
    has_sensors_ = true;

    fuchsia::ui::input::SensorDescriptorPtr acc_base =
        fuchsia::ui::input::SensorDescriptor::New();
    acc_base->type = fuchsia::ui::input::SensorType::ACCELEROMETER;
    acc_base->loc = fuchsia::ui::input::SensorLocation::BASE;
    sensor_descriptors_[kParadiseAccBase] = std::move(acc_base);

    fuchsia::ui::input::SensorDescriptorPtr acc_lid =
        fuchsia::ui::input::SensorDescriptor::New();
    acc_lid->type = fuchsia::ui::input::SensorType::ACCELEROMETER;
    acc_lid->loc = fuchsia::ui::input::SensorLocation::LID;
    sensor_descriptors_[kParadiseAccLid] = std::move(acc_lid);

    sensor_report_ = fuchsia::ui::input::InputReport::New();
    sensor_report_->sensor = fuchsia::ui::input::SensorReport::New();
  } else if (protocol == Protocol::EyoyoTouch) {
    FXL_VLOG(2) << "Device " << name() << " has touchscreen";
    has_touchscreen_ = true;
    touchscreen_descriptor_ = fuchsia::ui::input::TouchscreenDescriptor::New();

    touchscreen_descriptor_->x.range.min = 0;
    touchscreen_descriptor_->x.range.max = EYOYO_X_MAX;
    touchscreen_descriptor_->x.resolution = 1;

    touchscreen_descriptor_->y.range.min = 0;
    touchscreen_descriptor_->y.range.max = EYOYO_Y_MAX;
    touchscreen_descriptor_->y.resolution = 1;

    // TODO(jpoichet) do not hardcode this
    touchscreen_descriptor_->max_finger_id = 255;

    touchscreen_report_ = fuchsia::ui::input::InputReport::New();
    touchscreen_report_->touchscreen =
        fuchsia::ui::input::TouchscreenReport::New();

    touch_device_type_ = TouchDeviceType::EYOYO;
  } else if (protocol == Protocol::LightSensor) {
    FXL_VLOG(2) << "Device " << name() << " has an ambient light sensor";
    sensor_device_type_ = SensorDeviceType::AMBIENT_LIGHT;
    has_sensors_ = true;

    fuchsia::ui::input::SensorDescriptorPtr desc =
        fuchsia::ui::input::SensorDescriptor::New();
    desc->type = fuchsia::ui::input::SensorType::LIGHTMETER;
    desc->loc = fuchsia::ui::input::SensorLocation::UNKNOWN;
    sensor_descriptors_[kAmbientLight] = std::move(desc);

    sensor_report_ = fuchsia::ui::input::InputReport::New();
    sensor_report_->sensor = fuchsia::ui::input::SensorReport::New();
  } else if (protocol == Protocol::EyoyoTouch) {
    FXL_VLOG(2) << "Device " << name() << " has touchscreen";
    has_touchscreen_ = true;
    touchscreen_descriptor_ = fuchsia::ui::input::TouchscreenDescriptor::New();

    touchscreen_descriptor_->x.range.min = 0;
    touchscreen_descriptor_->x.range.max = EYOYO_X_MAX;
    touchscreen_descriptor_->x.resolution = 1;

    touchscreen_descriptor_->y.range.min = 0;
    touchscreen_descriptor_->y.range.max = EYOYO_Y_MAX;
    touchscreen_descriptor_->y.resolution = 1;

    // TODO(jpoichet) do not hardcode this
    touchscreen_descriptor_->max_finger_id = 255;

    touchscreen_report_ = fuchsia::ui::input::InputReport::New();
    touchscreen_report_->touchscreen =
        fuchsia::ui::input::TouchscreenReport::New();

    touch_device_type_ = TouchDeviceType::EYOYO;
  } else if (protocol == Protocol::Ft3x27Touch) {
    FXL_VLOG(2) << "Device " << name() << " has a touchscreen";
    has_touchscreen_ = true;
    touchscreen_descriptor_ = fuchsia::ui::input::TouchscreenDescriptor::New();
    touchscreen_descriptor_->x.range.min = 0;
    touchscreen_descriptor_->x.range.max = FT3X27_X_MAX;
    touchscreen_descriptor_->x.resolution = 1;
    touchscreen_descriptor_->y.range.min = 0;
    touchscreen_descriptor_->y.range.max = FT3X27_Y_MAX;
    touchscreen_descriptor_->y.resolution = 1;

    // TODO(SCN-867) Use HID parsing for all touch devices
    // will remove the need for this hardcoding
    touchscreen_descriptor_->max_finger_id = 255;

    touchscreen_report_ = fuchsia::ui::input::InputReport::New();
    touchscreen_report_->touchscreen =
        fuchsia::ui::input::TouchscreenReport::New();

    touch_device_type_ = TouchDeviceType::FT3X27;
  } else {
    FXL_VLOG(2) << "Device " << name() << " has unsupported HID device";
    return false;
  }

  event_ = hid_decoder_->GetEvent();
  if (!event_)
    return false;

  NotifyRegistry();
  return true;
}

void InputInterpreter::NotifyRegistry() {
  if (has_sensors_) {
    FXL_DCHECK(kMaxSensorCount == sensor_descriptors_.size());
    FXL_DCHECK(kMaxSensorCount == sensor_devices_.size());
    for (size_t i = 0; i < kMaxSensorCount; ++i) {
      if (sensor_descriptors_[i]) {
        fuchsia::ui::input::DeviceDescriptor descriptor;
        zx_status_t status =
            fidl::Clone(sensor_descriptors_[i], &descriptor.sensor);
        FXL_DCHECK(status == ZX_OK)
            << "Sensor descriptor: clone failed (status=" << status << ")";
        registry_->RegisterDevice(std::move(descriptor),
                                  sensor_devices_[i].NewRequest());
      }
    }
    // Sensor devices can't be anything else, so don't bother with other types.
    return;
  }

  fuchsia::ui::input::DeviceDescriptor descriptor;
  if (has_keyboard_) {
    fidl::Clone(keyboard_descriptor_, &descriptor.keyboard);
  }
  if (has_mouse_) {
    fidl::Clone(mouse_descriptor_, &descriptor.mouse);
  }
  if (has_stylus_) {
    fidl::Clone(stylus_descriptor_, &descriptor.stylus);
  }
  if (has_touchscreen_) {
    fidl::Clone(touchscreen_descriptor_, &descriptor.touchscreen);
  }
  registry_->RegisterDevice(std::move(descriptor), input_device_.NewRequest());
}

bool InputInterpreter::Read(bool discard) {
  TRACE_DURATION("input", "hid_read");

  // If positive |rc| is the number of bytes read. If negative the error
  // while reading.
  int rc = 1;
  auto report = hid_decoder_->Read(&rc);

  if (rc < 1) {
    FXL_LOG(ERROR) << "Failed to read from input: " << rc << " for " << name();
    // TODO(cpu) check whether the device was actually closed or not.
    return false;
  }

  // TODO(emircan): Consider removing all async events and adding durations and
  // flows instead.
  if (has_keyboard_) {
    hardcoded_.ParseKeyboardReport(report.data(), rc, keyboard_report_.get());
    if (!discard) {
      TRACE_FLOW_BEGIN("input", "hid_read_to_listener",
                       keyboard_report_->trace_id);
      TRACE_ASYNC_BEGIN("input", "dispatch_1_report_to_listener",
                        keyboard_report_->trace_id, "device_type", "keyboard");
      input_device_->DispatchReport(CloneReport(*keyboard_report_));
    }
  }

  if (has_buttons_) {
    if (!hardcoded_.ParseButtonsReport(report.data(), rc,
                                       buttons_report_.get()))
      return false;

    if (!discard) {
      TRACE_FLOW_BEGIN("input", "hid_read_to_listener",
                       buttons_report_->trace_id);
      TRACE_ASYNC_BEGIN("input", "dispatch_1_report_to_listener",
                        buttons_report_->trace_id, "device_type", "buttons");
      input_device_->DispatchReport(CloneReport(*buttons_report_));
    }
  }

  switch (mouse_device_type_) {
    case MouseDeviceType::BOOT:
      hardcoded_.ParseMouseReport(report.data(), rc, mouse_report_.get());
      if (!discard) {
        TRACE_FLOW_BEGIN("input", "hid_read_to_listener",
                         mouse_report_->trace_id);
        TRACE_ASYNC_BEGIN("inputinput", "dispatch_1_report_to_listener",
                          mouse_report_->trace_id, "device_type", "mouse");
        input_device_->DispatchReport(CloneReport(*mouse_report_));
      }
      break;
    case MouseDeviceType::TOUCH:
      if (ParseTouchpadReport(report.data(), rc, mouse_report_.get())) {
        if (!discard) {
          TRACE_FLOW_BEGIN("input", "hid_read_to_listener",
                           mouse_report_->trace_id);
          TRACE_ASYNC_BEGIN("input", "dispatch_1_report_to_listener",
                            mouse_report_->trace_id, "device_type", "touchpad");
          input_device_->DispatchReport(CloneReport(*mouse_report_));
        }
      }
      break;
    case MouseDeviceType::HID:
      if (mouse_.ParseReport(report.data(), rc, mouse_report_.get())) {
        if (!discard) {
          TRACE_FLOW_BEGIN("input", "hid_read_to_listener",
                           mouse_report_->trace_id);
          TRACE_ASYNC_BEGIN("input", "dispatch_1_report_to_listener",
                            mouse_report_->trace_id, "device_type", "mouse");
          mouse_report_->event_time = InputEventTimestampNow();
          mouse_report_->trace_id = TRACE_NONCE();
          input_device_->DispatchReport(CloneReport(*mouse_report_));
        }
      }
      break;
    case MouseDeviceType::PARADISEv1:
      if (hardcoded_.ParseParadiseTouchpadReportV1(report.data(), rc,
                                                   mouse_report_.get())) {
        if (!discard) {
          TRACE_FLOW_BEGIN("input", "hid_read_to_listener",
                           mouse_report_->trace_id);
          TRACE_ASYNC_BEGIN("input", "dispatch_1_report_to_listener",
                            mouse_report_->trace_id, "device_type", "touchpad");
          input_device_->DispatchReport(CloneReport(*mouse_report_));
        }
      }
      break;
    case MouseDeviceType::PARADISEv2:
      if (hardcoded_.ParseParadiseTouchpadReportV2(report.data(), rc,
                                                   mouse_report_.get())) {
        if (!discard) {
          TRACE_FLOW_BEGIN("input", "hid_read_to_listener",
                           mouse_report_->trace_id);
          TRACE_ASYNC_BEGIN("input", "dispatch_1_report_to_listener",
                            mouse_report_->trace_id, "device_type", "touchpad");
          input_device_->DispatchReport(CloneReport(*mouse_report_));
        }
      }
      break;
    case MouseDeviceType::GAMEPAD:
      // TODO(cpu): remove this once we have a good way to test gamepad.
      if (hardcoded_.ParseGamepadMouseReport(report.data(), rc,
                                             mouse_report_.get())) {
        if (!discard) {
          TRACE_FLOW_BEGIN("input", "hid_read_to_listener",
                           mouse_report_->trace_id);
          TRACE_ASYNC_BEGIN("input", "dispatch_1_report_to_listener",
                            mouse_report_->trace_id, "device_type", "gamepad");
          input_device_->DispatchReport(CloneReport(*mouse_report_));
        }
      }
      break;
    case MouseDeviceType::NONE:
      break;
  }

  switch (touch_device_type_) {
    case TouchDeviceType::HID:
      if (ParseTouchscreenReport(report.data(), rc,
                                 touchscreen_report_.get())) {
        if (!discard) {
          TRACE_FLOW_BEGIN("input", "hid_read_to_listener",
                           touchscreen_report_->trace_id);
          TRACE_ASYNC_BEGIN("input", "dispatch_1_report_to_listener",
                            touchscreen_report_->trace_id, "device_type",
                            "touchscreen");
          input_device_->DispatchReport(CloneReport(*touchscreen_report_));
        }
      }
      break;
    case TouchDeviceType::ACER12:
      if (report[0] == ACER12_RPT_ID_STYLUS) {
        if (hardcoded_.ParseAcer12StylusReport(report.data(), rc,
                                               stylus_report_.get())) {
          if (!discard) {
            TRACE_FLOW_BEGIN("input", "hid_read_to_listener",
                             touchscreen_report_->trace_id);
            TRACE_ASYNC_BEGIN("input", "dispatch_1_report_to_listener",
                              stylus_report_->trace_id, "device_type",
                              "stylus");
            input_device_->DispatchReport(CloneReport(*stylus_report_));
          }
        }
      } else if (report[0] == ACER12_RPT_ID_TOUCH) {
        if (hardcoded_.ParseAcer12TouchscreenReport(
                report.data(), rc, touchscreen_report_.get())) {
          if (!discard) {
            TRACE_FLOW_BEGIN("input", "hid_read_to_listener",
                             touchscreen_report_->trace_id);
            TRACE_ASYNC_BEGIN("input", "dispatch_1_report_to_listener",
                              touchscreen_report_->trace_id, "device_type",
                              "touchscreen");
            input_device_->DispatchReport(CloneReport(*touchscreen_report_));
          }
        }
      }
      break;

    case TouchDeviceType::SAMSUNG:
      if (report[0] == SAMSUNG_RPT_ID_TOUCH) {
        if (hardcoded_.ParseSamsungTouchscreenReport(
                report.data(), rc, touchscreen_report_.get())) {
          if (!discard) {
            TRACE_FLOW_BEGIN("input", "hid_read_to_listener",
                             touchscreen_report_->trace_id);
            TRACE_ASYNC_BEGIN("input", "dispatch_1_report_to_listener",
                              touchscreen_report_->trace_id, "device_type",
                              "touchscreen");
            input_device_->DispatchReport(CloneReport(*touchscreen_report_));
          }
        }
      }
      break;

    case TouchDeviceType::PARADISEv1:
      if (report[0] == PARADISE_RPT_ID_TOUCH) {
        if (hardcoded_.ParseParadiseTouchscreenReportV1(
                report.data(), rc, touchscreen_report_.get())) {
          if (!discard) {
            TRACE_FLOW_BEGIN("input", "hid_read_to_listener",
                             touchscreen_report_->trace_id);
            TRACE_ASYNC_BEGIN("input", "dispatch_1_report_to_listener",
                              touchscreen_report_->trace_id, "device_type",
                              "touchscreen");
            input_device_->DispatchReport(CloneReport(*touchscreen_report_));
          }
        }
      }
      break;
    case TouchDeviceType::PARADISEv2:
      if (report[0] == PARADISE_RPT_ID_TOUCH) {
        if (hardcoded_.ParseParadiseTouchscreenReportV2(
                report.data(), rc, touchscreen_report_.get())) {
          if (!discard) {
            TRACE_FLOW_BEGIN("input", "hid_read_to_listener",
                             touchscreen_report_->trace_id);
            TRACE_ASYNC_BEGIN("input", "dispatch_1_report_to_listener",
                              touchscreen_report_->trace_id, "device_type",
                              "touchscreen");
            input_device_->DispatchReport(CloneReport(*touchscreen_report_));
          }
        }
      } else if (report[0] == PARADISE_RPT_ID_STYLUS) {
        if (hardcoded_.ParseParadiseStylusReport(report.data(), rc,
                                                 stylus_report_.get())) {
          if (!discard) {
            TRACE_FLOW_BEGIN("input", "hid_read_to_listener",
                             stylus_report_->trace_id);
            TRACE_ASYNC_BEGIN("input", "dispatch_1_report_to_listener",
                              stylus_report_->trace_id, "device_type",
                              "stylus");
            input_device_->DispatchReport(CloneReport(*stylus_report_));
          }
        }
      }
      break;
    case TouchDeviceType::PARADISEv3:
      if (report[0] == PARADISE_RPT_ID_TOUCH) {
        // Paradise V3 uses the same touchscreen report as v1.
        if (hardcoded_.ParseParadiseTouchscreenReportV1(
                report.data(), rc, touchscreen_report_.get())) {
          if (!discard) {
            TRACE_FLOW_BEGIN("input", "hid_read_to_listener",
                             touchscreen_report_->trace_id);
            TRACE_ASYNC_BEGIN("input", "dispatch_1_report_to_listener",
                              touchscreen_report_->trace_id, "device_type",
                              "touchscreen");
            input_device_->DispatchReport(CloneReport(*touchscreen_report_));
          }
        }
      } else if (report[0] == PARADISE_RPT_ID_STYLUS) {
        if (hardcoded_.ParseParadiseStylusReport(report.data(), rc,
                                                 stylus_report_.get())) {
          if (!discard) {
            TRACE_FLOW_BEGIN("input", "hid_read_to_listener",
                             stylus_report_->trace_id);
            TRACE_ASYNC_BEGIN("input", "dispatch_1_report_to_listener",
                              stylus_report_->trace_id, "device_type",
                              "stylus");
            input_device_->DispatchReport(CloneReport(*stylus_report_));
          }
        }
      }
      break;
    case TouchDeviceType::EGALAX:
      if (report[0] == EGALAX_RPT_ID_TOUCH) {
        if (hardcoded_.ParseEGalaxTouchscreenReport(
                report.data(), rc, touchscreen_report_.get())) {
          if (!discard) {
            TRACE_FLOW_BEGIN("input", "hid_read_to_listener",
                             touchscreen_report_->trace_id);
            TRACE_ASYNC_BEGIN("input", "dispatch_1_report_to_listener",
                              touchscreen_report_->trace_id, "device_type",
                              "touchscreen");
            input_device_->DispatchReport(CloneReport(*touchscreen_report_));
          }
        }
      }
      break;

    case TouchDeviceType::EYOYO:
      if (report[0] == EYOYO_RPT_ID_TOUCH) {
        if (hardcoded_.ParseEyoyoTouchscreenReport(report.data(), rc,
                                                   touchscreen_report_.get())) {
          if (!discard) {
            TRACE_FLOW_BEGIN("input", "hid_read_to_listener",
                             touchscreen_report_->trace_id);
            TRACE_ASYNC_BEGIN("input", "dispatch_1_report_to_listener",
                              touchscreen_report_->trace_id, "device_type",
                              "touchscreen");
            input_device_->DispatchReport(CloneReport(*touchscreen_report_));
          }
        }
      }
      break;
    case TouchDeviceType::FT3X27:
      if (report[0] == FT3X27_RPT_ID_TOUCH) {
        if (hardcoded_.ParseFt3x27TouchscreenReport(
                report.data(), rc, touchscreen_report_.get())) {
          if (!discard) {
            TRACE_FLOW_BEGIN("input", "hid_read_to_listener",
                             touchscreen_report_->trace_id);
            TRACE_ASYNC_BEGIN("input", "dispatch_1_report_to_listener",
                              touchscreen_report_->trace_id, "device_type",
                              "touchscreen");
            input_device_->DispatchReport(CloneReport(*touchscreen_report_));
          }
        }
      }
      break;

    default:
      break;
  }

  switch (sensor_device_type_) {
    case SensorDeviceType::PARADISE:
      if (hardcoded_.ParseParadiseSensorReport(report.data(), rc, &sensor_idx_,
                                               sensor_report_.get())) {
        if (!discard) {
          FXL_DCHECK(sensor_idx_ < kMaxSensorCount);
          FXL_DCHECK(sensor_devices_[sensor_idx_]);
          TRACE_FLOW_BEGIN("input", "hid_read_to_listener",
                           sensor_report_->trace_id);
          TRACE_ASYNC_BEGIN("input", "dispatch_1_report_to_listener",
                            sensor_report_->trace_id, "device_type", "sensor");
          sensor_devices_[sensor_idx_]->DispatchReport(
              CloneReport(*sensor_report_));
        }
      }
      break;
    case SensorDeviceType::AMBIENT_LIGHT:
      if (hardcoded_.ParseAmbientLightSensorReport(
              report.data(), rc, &sensor_idx_, sensor_report_.get())) {
        if (!discard) {
          FXL_DCHECK(sensor_idx_ < kMaxSensorCount);
          FXL_DCHECK(sensor_devices_[sensor_idx_]);
          TRACE_FLOW_BEGIN("input", "hid_read_to_listener",
                           sensor_report_->trace_id);
          TRACE_ASYNC_BEGIN("input", "dispatch_1_report_to_listener",
                            sensor_report_->trace_id, "device_type",
                            "ambient_light");
          sensor_devices_[sensor_idx_]->DispatchReport(
              CloneReport(*sensor_report_));
        }
      }
      break;
    default:
      break;
  }

  return true;
}

// This logic converts the multi-finger report from the touchpad into
// a mouse report. It does this by only tracking the first finger that
// is placed down, and converting the absolution finger position into
// relative X and Y movements. All other fingers besides the tracking
// finger are ignored.
bool InputInterpreter::ParseTouchpadReport(
    uint8_t* report, size_t len,
    fuchsia::ui::input::InputReport* mouse_report) {
  Touchscreen::Report touchpad;
  if (!ParseReport(report, len, &touchpad)) {
    return false;
  }
  mouse_report->event_time = InputEventTimestampNow();
  mouse_report->trace_id = TRACE_NONCE();
  mouse_report->mouse->rel_x = 0;
  mouse_report->mouse->rel_y = 0;
  mouse_report->mouse->pressed_buttons = 0;

  // If all fingers are lifted reset our tracking finger.
  if (touchpad.contact_count == 0) {
    has_touch_ = false;
    tracking_finger_was_lifted_ = true;
    return true;
  }

  // If we don't have a tracking finger then set one.
  if (!has_touch_) {
    has_touch_ = true;
    tracking_finger_was_lifted_ = false;
    tracking_finger_id_ = touchpad.contacts[0].id;

    mouse_abs_x_ = touchpad.contacts[0].x;
    mouse_abs_y_ = touchpad.contacts[0].y;
    return true;
  }

  // Find the finger we are tracking.
  Touchscreen::ContactReport* contact = nullptr;
  for (size_t i = 0; i < touchpad.contact_count; i++) {
    if (touchpad.contacts[i].id == tracking_finger_id_) {
      contact = &touchpad.contacts[i];
      break;
    }
  }

  // If our tracking finger isn't pressed return early.
  if (contact == nullptr) {
    tracking_finger_was_lifted_ = true;
    return true;
  }

  // If our tracking finger was lifted then reset the abs values otherwise
  // the pointer will jump rapidly.
  if (tracking_finger_was_lifted_) {
    tracking_finger_was_lifted_ = false;
    mouse_abs_x_ = contact->x;
    mouse_abs_y_ = contact->y;
  }

  // The touch driver returns in units of 10^-5m, but the resolution expected
  // by |mouse_report_| is 10^-3.
  mouse_report->mouse->rel_x = (contact->x - mouse_abs_x_) / 100;
  mouse_report->mouse->rel_y = (contact->y - mouse_abs_y_) / 100;

  mouse_report->mouse->pressed_buttons =
      touchpad.button ? fuchsia::ui::input::kMouseButtonPrimary : 0;

  mouse_abs_x_ = touchpad.contacts[0].x;
  mouse_abs_y_ = touchpad.contacts[0].y;

  return true;
}

bool InputInterpreter::ParseTouchscreenReport(
    uint8_t* report, size_t len,
    fuchsia::ui::input::InputReport* touchscreen_report) {
  Touchscreen::Report touchscreen;
  if (!ParseReport(report, len, &touchscreen)) {
    return false;
  }
  touchscreen_report->event_time = InputEventTimestampNow();
  touchscreen_report->trace_id = TRACE_NONCE();
  touchscreen_report->touchscreen->touches.resize(touchscreen.contact_count);

  for (size_t i = 0; i < touchscreen.contact_count; ++i) {
    fuchsia::ui::input::Touch touch;
    touch.finger_id = touchscreen.contacts[i].id;
    touch.x = touchscreen.contacts[i].x;
    touch.y = touchscreen.contacts[i].y;
    // TODO(SCN-1188): Add support for contact ellipse.
    touch.width = 5;
    touch.height = 5;
    touchscreen_report->touchscreen->touches.at(i) = std::move(touch);
  }

  return true;
}

Protocol ExtractProtocol(hid::Usage input) {
  using ::hid::usage::Consumer;
  using ::hid::usage::Digitizer;
  using ::hid::usage::GenericDesktop;
  using ::hid::usage::Page;
  using ::hid::usage::Sensor;
  struct {
    hid::Usage usage;
    Protocol protocol;
  } usage_to_protocol[] = {
      {{static_cast<uint16_t>(Page::kSensor),
        static_cast<uint32_t>(Sensor::kAmbientLight)},
       Protocol::LightSensor},
      {{static_cast<uint16_t>(Page::kConsumer),
        static_cast<uint32_t>(Consumer::kConsumerControl)},
       Protocol::Buttons},
      {{static_cast<uint16_t>(Page::kDigitizer),
        static_cast<uint32_t>(Digitizer::kTouchScreen)},
       Protocol::Touch},
      {{static_cast<uint16_t>(Page::kDigitizer),
        static_cast<uint32_t>(Digitizer::kTouchPad)},
       Protocol::Touchpad},
      {{static_cast<uint16_t>(Page::kGenericDesktop),
        static_cast<uint32_t>(GenericDesktop::kMouse)},
       Protocol::Mouse},
      // Add more sensors here
  };
  for (auto& j : usage_to_protocol) {
    if (input.page == j.usage.page && input.usage == j.usage.usage) {
      return j.protocol;
    }
  }
  return Protocol::Other;
}

bool InputInterpreter::ConsumeDescriptor(Device::Descriptor* descriptor) {
  protocol_ = descriptor->protocol;
  if (descriptor->has_keyboard) {
    if (has_keyboard_) {
      FXL_LOG(ERROR) << name() << " HID device defines multiple keyboards";
      return false;
    }
    has_keyboard_ = true;
    keyboard_descriptor_ = std::move(descriptor->keyboard_descriptor);
  }
  if (descriptor->has_buttons) {
    if (has_buttons_) {
      FXL_LOG(ERROR) << name() << " HID device defines multiple buttons";
      return false;
    }
    has_buttons_ = true;
    buttons_descriptor_ = std::move(descriptor->buttons_descriptor);
  }
  if (descriptor->has_mouse) {
    if (has_mouse_) {
      FXL_LOG(ERROR) << name() << " HID device defines multiple mice";
      return false;
    }
    has_mouse_ = true;
    mouse_device_type_ = descriptor->mouse_type;
    mouse_descriptor_ = std::move(descriptor->mouse_descriptor);
  }
  if (descriptor->has_stylus) {
    if (has_stylus_) {
      FXL_LOG(ERROR) << name() << " HID device defines multiple styluses";
      return false;
    }
    has_stylus_ = true;
    stylus_descriptor_ = std::move(descriptor->stylus_descriptor);
  }
  if (descriptor->has_touchscreen) {
    if (has_touchscreen_) {
      FXL_LOG(ERROR) << name() << " HID device defines multiple touchscreens";
      return false;
    }
    has_touchscreen_ = true;
    touch_device_type_ = descriptor->touch_type;
    touchscreen_descriptor_ = std::move(descriptor->touchscreen_descriptor);
  }
  if (descriptor->has_sensor) {
    has_sensors_ = true;
    sensor_device_type_ = descriptor->sensor_type;
    sensor_descriptors_[descriptor->sensor_id] =
        std::move(descriptor->sensor_descriptor);
  }
  return true;
}

bool InputInterpreter::ParseProtocol() {
  HidDecoder::BootMode boot_mode = hid_decoder_->ReadBootMode();
  // For most keyboards and mouses Zircon requests the boot protocol
  // which has a fixed layout. This covers the following two cases:
  if (boot_mode == HidDecoder::BootMode::KEYBOARD) {
    protocol_ = Protocol::Keyboard;
    return true;
  }
  if (boot_mode == HidDecoder::BootMode::MOUSE) {
    protocol_ = Protocol::BootMouse;
    return true;
  }

  // For the rest of devices (fuchsia_hardware_input_BootProtocol_NONE) we need
  // to parse the report descriptor. The legacy method involves memcmp() of
  // known descriptors which cover the next 8 devices:

  int desc_size;
  auto desc = hid_decoder_->ReadReportDescriptor(&desc_size);
  if (desc_size == 0) {
    return false;
  }

  if (is_acer12_touch_report_desc(desc.data(), desc.size())) {
    protocol_ = Protocol::Acer12Touch;
    return true;
  }
  if (is_samsung_touch_report_desc(desc.data(), desc.size())) {
    hid_decoder_->SetupDevice(HidDecoder::Device::SAMSUNG);
    protocol_ = Protocol::SamsungTouch;
    return true;
  }
  if (is_paradise_touch_report_desc(desc.data(), desc.size())) {
    protocol_ = Protocol::ParadiseV1Touch;
    return true;
  }
  if (is_paradise_touch_v2_report_desc(desc.data(), desc.size())) {
    protocol_ = Protocol::ParadiseV2Touch;
    return true;
  }
  if (is_paradise_touch_v3_report_desc(desc.data(), desc.size())) {
    protocol_ = Protocol::ParadiseV3Touch;
    return true;
  }
  if (USE_TOUCHPAD_HARDCODED_REPORTS) {
    if (is_paradise_touchpad_v1_report_desc(desc.data(), desc.size())) {
      protocol_ = Protocol::ParadiseV1TouchPad;
      return true;
    }
    if (is_paradise_touchpad_v2_report_desc(desc.data(), desc.size())) {
      protocol_ = Protocol::ParadiseV2TouchPad;
      return true;
    }
  }
  if (is_egalax_touchscreen_report_desc(desc.data(), desc.size())) {
    protocol_ = Protocol::EgalaxTouch;
    return true;
  }
  if (is_paradise_sensor_report_desc(desc.data(), desc.size())) {
    protocol_ = Protocol::ParadiseSensor;
    return true;
  }
  if (is_eyoyo_touch_report_desc(desc.data(), desc.size())) {
    hid_decoder_->SetupDevice(HidDecoder::Device::EYOYO);
    protocol_ = Protocol::EyoyoTouch;
    return true;
  }
  // TODO(SCN-867) Use HID parsing for all touch devices
  // will remove the need for this
  if (is_ft3x27_touch_report_desc(desc.data(), desc.size())) {
    hid_decoder_->SetupDevice(HidDecoder::Device::FT3X27);
    protocol_ = Protocol::Ft3x27Touch;
    return true;
  }

  // For the rest of devices we use the new way; with the hid-parser
  // library.

  hid::DeviceDescriptor* dev_desc = nullptr;
  auto parse_res =
      hid::ParseReportDescriptor(desc.data(), desc.size(), &dev_desc);
  if (parse_res != hid::ParseResult::kParseOk) {
    FXL_LOG(ERROR) << "hid-parser: error " << int(parse_res)
                   << " parsing report descriptor for " << name();
    return false;
  }

  auto count = dev_desc->rep_count;
  if (count == 0) {
    FXL_LOG(ERROR) << "no report descriptors for " << name();
    return false;
  }

  // Find the first input report.
  const hid::ReportDescriptor* input_desc = nullptr;
  for (size_t rep = 0; rep < count; rep++) {
    const hid::ReportDescriptor* desc = &dev_desc->report[rep];
    if (desc->input_count != 0) {
      input_desc = desc;
      break;
    }
  }

  if (input_desc == nullptr) {
    FXL_LOG(ERROR) << "no input report fields for " << name();
    return false;
  }

  // Traverse up the nested collections to the Application collection.
  auto collection = input_desc->input_fields[0].col;
  while (collection != nullptr) {
    if (collection->type == hid::CollectionType::kApplication) {
      break;
    }
    collection = collection->parent;
  }

  if (collection == nullptr) {
    FXL_LOG(ERROR) << "invalid hid collection for " << name();
    return false;
  }

  FXL_LOG(INFO) << "hid-parser succesful for " << name() << " with usage page "
                << collection->usage.page << " and usage "
                << collection->usage.usage;

  // Most modern gamepads report themselves as Joysticks. Madness.
  if (collection->usage.page == hid::usage::Page::kGenericDesktop &&
      collection->usage.usage == hid::usage::GenericDesktop::kJoystick &&
      hardcoded_.ParseGamepadDescriptor(input_desc->input_fields,
                                        input_desc->input_count)) {
    protocol_ = Protocol::Gamepad;
  } else {
    protocol_ = ExtractProtocol(collection->usage);
    switch (protocol_) {
      case Protocol::LightSensor:
        hardcoded_.ParseAmbientLightDescriptor(input_desc->input_fields,
                                               input_desc->input_count);
        break;
      case Protocol::Buttons:
        hardcoded_.ParseButtonsDescriptor(input_desc->input_fields,
                                          input_desc->input_count);
        break;
      case Protocol::Touchpad:
        // Fallthrough
      case Protocol::Touch: {
        bool success = ts_.ParseTouchscreenDescriptor(input_desc);
        if (!success) {
          FXL_LOG(ERROR) << "invalid touchscreen descriptor for " << name();
          return false;
        }
        break;
      }
      case Protocol::Mouse: {
        Device::Descriptor device_descriptor = {};
        if (!mouse_.ParseReportDescriptor(*input_desc, &device_descriptor)) {
          FXL_LOG(ERROR) << "invalid mouse descriptor for " << name();
          return false;
        }
        if (!ConsumeDescriptor(&device_descriptor)) {
          return false;
        }
        break;
      }
      // Add more protocols here
      default:
        return false;
    }
  }

  return true;
}

bool InputInterpreter::ParseReport(const uint8_t* report, size_t len,
                                   Touchscreen::Report* touchscreen) {
  if (report[0] != ts_.report_id()) {
    FXL_VLOG(0) << name() << " Touchscreen report "
                << static_cast<uint32_t>(report[0])
                << " does not match report id "
                << static_cast<uint32_t>(ts_.report_id());
    return false;
  }

  return ts_.ParseReport(report, len, touchscreen);
}

bool InputInterpreter::SetDescriptor(Touchscreen::Descriptor* touch_desc) {
  return ts_.SetDescriptor(touch_desc);
}

}  // namespace mozart
