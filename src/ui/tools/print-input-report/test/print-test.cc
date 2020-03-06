// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/input/report/llcpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/fidl-async/cpp/bind.h>
#include <lib/sync/completion.h>

#include <string>
#include <vector>

#include <ddk/device.h>
#include <fbl/auto_lock.h>
#include <fbl/mutex.h>
#include <gtest/gtest.h>
#include <hid/usages.h>

#include "src/ui/input/lib/hid-input-report/fidl.h"
#include "src/ui/input/testing/fake_input_report_device/fake.h"
#include "src/ui/tools/print-input-report/devices.h"
#include "src/ui/tools/print-input-report/printer.h"

namespace test {

namespace fuchsia_input_report = ::llcpp::fuchsia::input::report;

class FakePrinter : public print_input_report::Printer {
 public:
  void RealPrint(const char* format, va_list argptr) override {
    char buf[kMaxBufLen];
    vsprintf(buf, format, argptr);

    ASSERT_LT(current_string_index_, expected_strings_.size());
    const std::string& expected = expected_strings_[current_string_index_];
    current_string_index_++;

    // Check that we match the expected string.
    ASSERT_GT(expected.size(), indent_);
    int cmp = strcmp(buf, expected.c_str());
    if (cmp != 0) {
      printf("Wanted string: '%s'\n", expected.c_str());
      printf("Saw string:    '%s'\n", buf);
      ASSERT_TRUE(false);
    }

    // Print the string for easy debugging.
    vprintf(format, argptr);

    va_end(argptr);
  }

  void SetExpectedStrings(const std::vector<std::string>& strings) {
    current_string_index_ = 0;
    expected_strings_ = strings;
  }

 private:
  static constexpr size_t kMaxBufLen = 1024;
  size_t current_string_index_ = 0;
  std::vector<std::string> expected_strings_;
};

class PrintInputReport : public ::testing::Test {
 protected:
  virtual void SetUp() {
    // Make the channels and the fake device.
    zx::channel token_server, token_client;
    ASSERT_EQ(zx::channel::create(0, &token_server, &token_client), ZX_OK);
    fake_device_ = std::make_unique<fake_input_report_device::FakeInputDevice>();

    // Make and run the thread for the fake device's FIDL interface. This is necessary
    // because the interface is asyncronous and will block the dispatcher.
    // TODO(dgilhooley): When LLCPP supports async clients, make this test single threaded
    // with a dispatcher.
    loop_ = std::make_unique<async::Loop>(&kAsyncLoopConfigAttachToCurrentThread);
    ASSERT_EQ(loop_->StartThread("test-print-input-report-loop"), ZX_OK);
    fidl::Bind(loop_->dispatcher(), std::move(token_server), fake_device_.get());

    // Make the client.
    client_ = fuchsia_input_report::InputDevice::SyncClient(std::move(token_client));
  }

  virtual void TearDown() {
    loop_->Quit();
    loop_->JoinThreads();
  }

  std::unique_ptr<async::Loop> loop_;
  std::unique_ptr<fake_input_report_device::FakeInputDevice> fake_device_;
  std::optional<fuchsia_input_report::InputDevice::SyncClient> client_;
};

TEST_F(PrintInputReport, PrintMouseInputReport) {
  hid_input_report::MouseInputReport mouse = {};
  mouse.movement_x = 100;
  mouse.movement_y = 200;
  mouse.scroll_v = 100;

  mouse.num_buttons_pressed = 3;
  mouse.buttons_pressed[0] = 1;
  mouse.buttons_pressed[1] = 10;
  mouse.buttons_pressed[2] = 5;

  hid_input_report::InputReport report;
  report.report = mouse;

  fake_device_->SetReport(report);

  FakePrinter printer;
  printer.SetExpectedStrings(std::vector<std::string>{
      "Movement x: 00000100\n",
      "Movement y: 00000200\n",
      "Scroll v: 00000100\n",
      "Button 01 pressed\n",
      "Button 10 pressed\n",
      "Button 05 pressed\n",
      "\n",
  });
  print_input_report::PrintInputReport(&printer, &client_.value(), 1);
}

TEST_F(PrintInputReport, PrintMouseInputDescriptor) {
  hid_input_report::MouseDescriptor mouse = {};
  mouse.input = hid_input_report::MouseInputDescriptor();

  fuchsia_input_report::Axis axis;
  axis.unit = fuchsia_input_report::Unit::DISTANCE;
  axis.range.min = -100;
  axis.range.max = -100;
  mouse.input->movement_x = axis;

  axis.unit = fuchsia_input_report::Unit::NONE;
  axis.range.min = -200;
  axis.range.max = -200;
  mouse.input->movement_y = axis;

  mouse.input->num_buttons = 3;
  mouse.input->buttons[0] = 1;
  mouse.input->buttons[1] = 10;
  mouse.input->buttons[2] = 5;

  hid_input_report::ReportDescriptor descriptor;
  descriptor.descriptor = mouse;

  fake_device_->SetDescriptor(descriptor);

  FakePrinter printer;
  printer.SetExpectedStrings(std::vector<std::string>{
      "Mouse Descriptor:\n",
      "  Movement X:\n",
      "    Unit: DISTANCE\n",
      "    Min:      -100\n",
      "    Max:      -100\n",
      "  Movement Y:\n",
      "    Unit:     NONE\n",
      "    Min:      -200\n",
      "    Max:      -200\n",
      "  Button: 1\n",
      "  Button: 10\n",
      "  Button: 5\n",
  });

  print_input_report::PrintInputDescriptor(&printer, &client_.value());
}

TEST_F(PrintInputReport, PrintSensorInputDescriptor) {
  fuchsia_input_report::Axis axis;
  axis.unit = fuchsia_input_report::Unit::LINEAR_VELOCITY;
  axis.range.min = 0;
  axis.range.max = 1000;

  hid_input_report::SensorDescriptor sensor_desc = {};
  sensor_desc.input = hid_input_report::SensorInputDescriptor();
  sensor_desc.input->values[0].axis = axis;
  sensor_desc.input->values[0].type = fuchsia_input_report::SensorType::ACCELEROMETER_X;

  axis.unit = fuchsia_input_report::Unit::LUMINOUS_FLUX;
  sensor_desc.input->values[1].axis = axis;
  sensor_desc.input->values[1].type = fuchsia_input_report::SensorType::LIGHT_ILLUMINANCE;
  sensor_desc.input->num_values = 2;

  hid_input_report::ReportDescriptor desc;
  desc.descriptor = sensor_desc;

  fake_device_->SetDescriptor(desc);

  FakePrinter printer;
  printer.SetExpectedStrings(std::vector<std::string>{
      "Sensor Descriptor:\n",
      "  Value 00:\n",
      "    SensorType: ACCELEROMETER_X\n",
      "    Unit: LINEAR_VELOCITY\n",
      "    Min:         0\n",
      "    Max:      1000\n",
      "  Value 01:\n",
      "    SensorType: LIGHT_ILLUMINANCE\n",
      "    Unit: LUMINOUS_FLUX\n",
      "    Min:         0\n",
      "    Max:      1000\n",
  });

  print_input_report::PrintInputDescriptor(&printer, &client_.value());
}

TEST_F(PrintInputReport, PrintSensorInputReport) {
  hid_input_report::SensorInputReport sensor_report = {};
  sensor_report.values[0] = 100;
  sensor_report.values[1] = -100;
  sensor_report.num_values = 2;

  hid_input_report::InputReport report;
  report.report = sensor_report;

  fake_device_->SetReport(report);

  FakePrinter printer;
  printer.SetExpectedStrings(std::vector<std::string>{
      "Sensor[00]: 00000100\n",
      "Sensor[01]: -0000100\n",
      "\n",
  });

  print_input_report::PrintInputReport(&printer, &client_.value(), 1);
}

TEST_F(PrintInputReport, PrintTouchInputDescriptor) {
  hid_input_report::TouchDescriptor touch_desc = {};
  touch_desc.input = hid_input_report::TouchInputDescriptor();
  touch_desc.input->touch_type = fuchsia_input_report::TouchType::TOUCHSCREEN;

  touch_desc.input->max_contacts = 100;

  fuchsia_input_report::Axis axis;
  axis.unit = fuchsia_input_report::Unit::NONE;
  axis.range.min = 0;
  axis.range.max = 300;

  touch_desc.input->contacts[0].position_x = axis;

  axis.range.max = 500;
  touch_desc.input->contacts[0].position_y = axis;

  axis.range.max = 100;
  touch_desc.input->contacts[0].pressure = axis;

  touch_desc.input->num_contacts = 1;

  hid_input_report::ReportDescriptor desc;
  desc.descriptor = touch_desc;

  fake_device_->SetDescriptor(desc);

  FakePrinter printer;
  printer.SetExpectedStrings(std::vector<std::string>{
      "Touch Descriptor:\n",
      "  Touch Type: TOUCHSCREEN\n",
      "  Max Contacts: 100\n",
      "  Contact: 00\n",
      "    Position X:\n",
      "      Unit:     NONE\n",
      "      Min:         0\n",
      "      Max:       300\n",
      "    Position Y:\n",
      "      Unit:     NONE\n",
      "      Min:         0\n",
      "      Max:       500\n",
      "    Pressure:\n",
      "      Unit:     NONE\n",
      "      Min:         0\n",
      "      Max:       100\n",
  });

  print_input_report::PrintInputDescriptor(&printer, &client_.value());
}

TEST_F(PrintInputReport, PrintTouchInputReport) {
  hid_input_report::TouchInputReport touch_report = {};

  touch_report.num_contacts = 1;

  touch_report.contacts[0].contact_id = 10;
  touch_report.contacts[0].position_x = 123;
  touch_report.contacts[0].position_y = 234;
  touch_report.contacts[0].pressure = 345;
  touch_report.contacts[0].contact_width = 678;
  touch_report.contacts[0].contact_height = 789;

  hid_input_report::InputReport report;
  report.report = touch_report;

  fake_device_->SetReport(report);

  FakePrinter printer;
  printer.SetExpectedStrings(std::vector<std::string>{
      "Contact ID: 10\n",
      "  Position X:     00000123\n",
      "  Position Y:     00000234\n",
      "  Pressure:       00000345\n",
      "  Contact Width:  00000678\n",
      "  Contact Height: 00000789\n",
      "\n",
  });

  print_input_report::PrintInputReport(&printer, &client_.value(), 1);
}

TEST_F(PrintInputReport, PrintKeyboardDescriptor) {
  hid_input_report::KeyboardDescriptor keyboard_desc = {};

  keyboard_desc.input = hid_input_report::KeyboardInputDescriptor();
  keyboard_desc.input->num_keys = 3;
  keyboard_desc.input->keys[0] = llcpp::fuchsia::ui::input2::Key::A;
  keyboard_desc.input->keys[1] = llcpp::fuchsia::ui::input2::Key::UP;
  keyboard_desc.input->keys[2] = llcpp::fuchsia::ui::input2::Key::LEFT_SHIFT;

  keyboard_desc.output = hid_input_report::KeyboardOutputDescriptor();
  keyboard_desc.output->num_leds = 2;
  keyboard_desc.output->leds[0] = fuchsia_input_report::LedType::CAPS_LOCK;
  keyboard_desc.output->leds[1] = fuchsia_input_report::LedType::SCROLL_LOCK;

  hid_input_report::ReportDescriptor desc;
  desc.descriptor = keyboard_desc;

  fake_device_->SetDescriptor(desc);

  FakePrinter printer;
  printer.SetExpectedStrings(std::vector<std::string>{
      "Keyboard Descriptor:\n",
      "Input Report:\n",
      "  Key:        1\n",
      "  Key:       79\n",
      "  Key:       82\n",
      "Output Report:\n",
      "  Led: CAPS_LOCK\n",
      "  Led: SCROLL_LOCK\n",
  });

  print_input_report::PrintInputDescriptor(&printer, &client_.value());
}

TEST_F(PrintInputReport, PrintKeyboardInputReport) {
  hid_input_report::KeyboardInputReport keyboard_report = {};

  keyboard_report.num_pressed_keys = 3;
  keyboard_report.pressed_keys[0] = llcpp::fuchsia::ui::input2::Key::A;
  keyboard_report.pressed_keys[1] = llcpp::fuchsia::ui::input2::Key::UP;
  keyboard_report.pressed_keys[2] = llcpp::fuchsia::ui::input2::Key::LEFT_SHIFT;

  hid_input_report::InputReport report;
  report.report = keyboard_report;

  fake_device_->SetReport(report);

  FakePrinter printer;
  printer.SetExpectedStrings(std::vector<std::string>{
      "Keyboard Report\n",
      "  Key:        1\n",
      "  Key:       79\n",
      "  Key:       82\n",
      "\n",
  });

  print_input_report::PrintInputReport(&printer, &client_.value(), 1);
}

TEST_F(PrintInputReport, PrintKeyboardInputReportNoKeys) {
  hid_input_report::KeyboardInputReport keyboard_report = {};

  keyboard_report.num_pressed_keys = 0;

  hid_input_report::InputReport report;
  report.report = keyboard_report;

  fake_device_->SetReport(report);

  FakePrinter printer;
  printer.SetExpectedStrings(std::vector<std::string>{
      "Keyboard Report\n",
      "  No keys pressed\n",
      "\n",
  });

  print_input_report::PrintInputReport(&printer, &client_.value(), 1);
}

TEST_F(PrintInputReport, PrintConsumerControlDescriptor) {
  hid_input_report::ConsumerControlDescriptor descriptor = {};

  descriptor.input = hid_input_report::ConsumerControlInputDescriptor();
  descriptor.input->num_buttons = 3;
  descriptor.input->buttons[0] = fuchsia_input_report::ConsumerControlButton::VOLUME_UP;
  descriptor.input->buttons[1] = fuchsia_input_report::ConsumerControlButton::VOLUME_DOWN;
  descriptor.input->buttons[2] = fuchsia_input_report::ConsumerControlButton::REBOOT;

  hid_input_report::ReportDescriptor report_descriptor;
  report_descriptor.descriptor = descriptor;

  fake_device_->SetDescriptor(report_descriptor);

  FakePrinter printer;
  printer.SetExpectedStrings(std::vector<std::string>{
      "ConsumerControl Descriptor:\n",
      "Input Report:\n",
      "  Button:        VOLUME_UP\n",
      "  Button:      VOLUME_DOWN\n",
      "  Button:           REBOOT\n",
      "\n",
  });

  print_input_report::PrintInputDescriptor(&printer, &client_.value());
}

TEST_F(PrintInputReport, PrintConsumerControlReport) {
  hid_input_report::ConsumerControlInputReport report = {};

  report.num_pressed_buttons = 3;
  report.pressed_buttons[0] = fuchsia_input_report::ConsumerControlButton::VOLUME_UP;
  report.pressed_buttons[1] = fuchsia_input_report::ConsumerControlButton::VOLUME_DOWN;
  report.pressed_buttons[2] = fuchsia_input_report::ConsumerControlButton::REBOOT;

  hid_input_report::InputReport input_report;
  input_report.report = report;

  fake_device_->SetReport(input_report);

  FakePrinter printer;
  printer.SetExpectedStrings(std::vector<std::string>{
      "ConsumerControl Report\n",
      "  Button:        VOLUME_UP\n",
      "  Button:      VOLUME_DOWN\n",
      "  Button:           REBOOT\n",
      "\n",
  });

  print_input_report::PrintInputReport(&printer, &client_.value(), 1);
}

}  // namespace test
