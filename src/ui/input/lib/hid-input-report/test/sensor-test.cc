// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/input/lib/hid-input-report/sensor.h"

#include <variant>

#include <hid-parser/usages.h>
#include <hid/ambient-light.h>
#include <zxtest/zxtest.h>

#include "src/ui/input/lib/hid-input-report/device.h"

// Each test parses the report descriptor for the mouse and then sends one
// report to ensure that it has been parsed correctly.

namespace hid_input_report {

TEST(SensorTest, AmbientLight) {
  // Create the descriptor.
  hid::DeviceDescriptor* dev_desc = nullptr;
  const uint8_t* desc;
  size_t desc_size = get_ambient_light_report_desc(&desc);
  hid::ParseResult parse_res = hid::ParseReportDescriptor(desc, desc_size, &dev_desc);
  ASSERT_EQ(hid::ParseResult::kParseOk, parse_res);

  hid_input_report::Sensor sensor;

  // Parse the descriptor.
  EXPECT_EQ(hid_input_report::ParseResult::kOk, sensor.ParseReportDescriptor(dev_desc->report[1]));
  hid_input_report::ReportDescriptor report_descriptor = sensor.GetDescriptor();

  hid_input_report::SensorDescriptor* sensor_descriptor =
      std::get_if<hid_input_report::SensorDescriptor>(&report_descriptor.descriptor);
  ASSERT_NOT_NULL(sensor_descriptor);
  hid_input_report::SensorInputDescriptor* sensor_input_descriptor =
      &sensor_descriptor->input.value();

  // Check the descriptor.
  ASSERT_EQ(4, sensor_input_descriptor->num_values);

  ASSERT_EQ(sensor_input_descriptor->values[0].type,
            ::llcpp::fuchsia::input::report::SensorType::LIGHT_ILLUMINANCE);
  ASSERT_EQ(sensor_input_descriptor->values[0].axis.unit,
            ::llcpp::fuchsia::input::report::Unit::LUX);

  ASSERT_EQ(sensor_input_descriptor->values[1].type,
            ::llcpp::fuchsia::input::report::SensorType::LIGHT_RED);
  ASSERT_EQ(sensor_input_descriptor->values[1].axis.unit,
            ::llcpp::fuchsia::input::report::Unit::LUX);

  ASSERT_EQ(sensor_input_descriptor->values[2].type,
            ::llcpp::fuchsia::input::report::SensorType::LIGHT_BLUE);
  ASSERT_EQ(sensor_input_descriptor->values[2].axis.unit,
            ::llcpp::fuchsia::input::report::Unit::LUX);

  ASSERT_EQ(sensor_input_descriptor->values[3].type,
            ::llcpp::fuchsia::input::report::SensorType::LIGHT_GREEN);
  ASSERT_EQ(sensor_input_descriptor->values[3].axis.unit,
            ::llcpp::fuchsia::input::report::Unit::LUX);

  // Create the report.
  ambient_light_input_rpt_t report_data = {};
  // Values are arbitrarily chosen.
  constexpr int kIlluminanceTestVal = 10;
  constexpr int kRedTestVal = 101;
  constexpr int kBlueTestVal = 5;
  constexpr int kGreenTestVal = 3;
  report_data.rpt_id = AMBIENT_LIGHT_RPT_ID_INPUT;
  report_data.illuminance = kIlluminanceTestVal;
  report_data.red = kRedTestVal;
  report_data.blue = kBlueTestVal;
  report_data.green = kGreenTestVal;

  // Parse the report.
  hid_input_report::InputReport report = {};
  EXPECT_EQ(hid_input_report::ParseResult::kOk,
            sensor.ParseInputReport(reinterpret_cast<uint8_t*>(&report_data), sizeof(report_data),
                                    &report));

  hid_input_report::SensorInputReport* sensor_report =
      std::get_if<hid_input_report::SensorInputReport>(&report.report);
  ASSERT_NOT_NULL(sensor_report);
  EXPECT_EQ(4, sensor_report->num_values);

  // Check the report.
  // These will always match the ordering in the descriptor.
  constexpr uint32_t kLightUnitConversion = 100;
  EXPECT_EQ(kIlluminanceTestVal * kLightUnitConversion, sensor_report->values[0]);
  EXPECT_EQ(kRedTestVal * kLightUnitConversion, sensor_report->values[1]);
  EXPECT_EQ(kBlueTestVal * kLightUnitConversion, sensor_report->values[2]);
  EXPECT_EQ(kGreenTestVal * kLightUnitConversion, sensor_report->values[3]);
}
}  // namespace hid_input_report
