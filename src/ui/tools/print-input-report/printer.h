// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_TOOLS_PRINT_INPUT_REPORT_PRINTER_H_
#define SRC_UI_TOOLS_PRINT_INPUT_REPORT_PRINTER_H_

#include <fuchsia/input/report/llcpp/fidl.h>
#include <stdarg.h>
#include <stdio.h>

#include <string>

namespace print_input_report {

namespace fuchsia_input_report = ::llcpp::fuchsia::input::report;

static_assert(static_cast<int>(fuchsia_input_report::Unit::NONE) == 0);
static_assert(static_cast<int>(fuchsia_input_report::Unit::OTHER) == 1);
static_assert(static_cast<int>(fuchsia_input_report::Unit::DISTANCE) == 2);
static_assert(static_cast<int>(fuchsia_input_report::Unit::WEIGHT) == 3);
static_assert(static_cast<int>(fuchsia_input_report::Unit::ROTATION) == 4);
static_assert(static_cast<int>(fuchsia_input_report::Unit::ANGULAR_VELOCITY) == 5);
static_assert(static_cast<int>(fuchsia_input_report::Unit::LINEAR_VELOCITY) == 6);
static_assert(static_cast<int>(fuchsia_input_report::Unit::ACCELERATION) == 7);
static_assert(static_cast<int>(fuchsia_input_report::Unit::MAGNETIC_FLUX) == 8);
static_assert(static_cast<int>(fuchsia_input_report::Unit::LUMINOUS_FLUX) == 9);
static_assert(static_cast<int>(fuchsia_input_report::Unit::PRESSURE) == 10);
static_assert(static_cast<int>(fuchsia_input_report::Unit::LUX) == 11);

// These strings must be ordered based on the enums in fuchsia.input.report/units.fidl.
const char* const kUnitStrings[] = {
    "NONE",
    "OTHER",
    "DISTANCE",
    "WEIGHT",
    "ROTATION",
    "ANGULAR_VELOCITY",
    "LINEAR_VELOCITY",
    "ACCELERATION",
    "MAGNETIC_FLUX",
    "LUMINOUS_FLUX",
    "PRESSURE",
    "LUX",
};

static_assert(static_cast<int>(fuchsia_input_report::SensorType::ACCELEROMETER_X) == 1);
static_assert(static_cast<int>(fuchsia_input_report::SensorType::ACCELEROMETER_Y) == 2);
static_assert(static_cast<int>(fuchsia_input_report::SensorType::ACCELEROMETER_Z) == 3);
static_assert(static_cast<int>(fuchsia_input_report::SensorType::MAGNETOMETER_X) == 4);
static_assert(static_cast<int>(fuchsia_input_report::SensorType::MAGNETOMETER_Y) == 5);
static_assert(static_cast<int>(fuchsia_input_report::SensorType::MAGNETOMETER_Z) == 6);
static_assert(static_cast<int>(fuchsia_input_report::SensorType::GYROSCOPE_X) == 7);
static_assert(static_cast<int>(fuchsia_input_report::SensorType::GYROSCOPE_Y) == 8);
static_assert(static_cast<int>(fuchsia_input_report::SensorType::GYROSCOPE_Z) == 9);
static_assert(static_cast<int>(fuchsia_input_report::SensorType::LIGHT_ILLUMINANCE) == 10);
static_assert(static_cast<int>(fuchsia_input_report::SensorType::LIGHT_RED) == 11);
static_assert(static_cast<int>(fuchsia_input_report::SensorType::LIGHT_GREEN) == 12);
static_assert(static_cast<int>(fuchsia_input_report::SensorType::LIGHT_BLUE) == 13);

// These strings must be ordered based on the enums in fuchsia.input.report/sensor.fidl.
const char* const kSensorTypeStrings[] = {
    "ERROR",          "ACCELEROMETER_X", "ACCELEROMETER_Y",   "ACCELEROMETER_Z",
    "MAGNETOMETER_X", "MAGNETOMETER_Y",  "MAGNETOMETER_Z",    "GYROSCOPE_X",
    "GYROSCOPE_Y",    "GYROSCOPE_Z",     "LIGHT_ILLUMINANCE", "LIGHT_RED",
    "LIGHT_GREEN",    "LIGHT_BLUE",
};

static_assert(static_cast<int>(fuchsia_input_report::TouchType::TOUCHSCREEN) == 1);
// These strings must be ordered based on the enums in fuchsia.input.report/touch.fidl.
const char* const kTouchTypeStrings[] = {
    "ERROR",
    "TOUCHSCREEN",
};

class Printer {
 public:
  Printer() = default;

  // Find the string related to the unit. If we are given a value that we do not
  // recognize, the string "NONE" will be returned and printed.
  static const char* UnitToString(fuchsia_input_report::Unit unit) {
    uint32_t unit_index = static_cast<uint32_t>(unit);
    if (unit_index >= countof(kUnitStrings)) {
      return kUnitStrings[0];
    }
    return kUnitStrings[unit_index];
  }

  // Find the string related to the sensor type. If we are given a value that we do not
  // recognize, the string "ERROR" will be returned and printed.
  static const char* SensorTypeToString(fuchsia_input_report::SensorType type) {
    uint32_t unit_index = static_cast<uint32_t>(type);
    if (unit_index >= countof(kSensorTypeStrings)) {
      return kSensorTypeStrings[0];
    }
    return kSensorTypeStrings[unit_index];
  }

  static const char* TouchTypeToString(fuchsia_input_report::TouchType type) {
    uint32_t unit_index = static_cast<uint32_t>(type);
    if (unit_index >= countof(kTouchTypeStrings)) {
      return kTouchTypeStrings[0];
    }
    return kTouchTypeStrings[unit_index];
  }

  void PrintAxis(fuchsia_input_report::Axis axis) {
    this->Print("Unit: %8s\n", UnitToString(axis.unit));
    this->Print("Min:  %8ld\n", axis.range.min);
    this->Print("Max:  %8ld\n", axis.range.max);
  }

  void PrintAxisIndented(fuchsia_input_report::Axis axis) {
    IncreaseIndent();
    this->Print("Unit: %8s\n", UnitToString(axis.unit));
    this->Print("Min:  %8ld\n", axis.range.min);
    this->Print("Max:  %8ld\n", axis.range.max);
    DecreaseIndent();
  }

  void Print(const char* format, ...) {
    std::string str_format(indent_, ' ');
    str_format += format;

    va_list argptr;
    va_start(argptr, format);
    RealPrint(str_format.c_str(), argptr);
    va_end(argptr);
  }

  void SetIndent(size_t indent) { indent_ = indent; }

  void IncreaseIndent() { indent_ += 2; }

  void DecreaseIndent() { indent_ -= 2; }

 protected:
  virtual void RealPrint(const char* format, va_list argptr) { vprintf(format, argptr); }
  size_t indent_ = 0;
};

}  // namespace print_input_report

#endif  // SRC_UI_TOOLS_PRINT_INPUT_REPORT_PRINTER_H_
