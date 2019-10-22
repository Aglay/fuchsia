// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef HID_INPUT_REPORT_SENSOR_H_
#define HID_INPUT_REPORT_SENSOR_H_

#include <hid-input-report/descriptors.h>
#include <hid-input-report/device.h>

namespace hid_input_report {

class Sensor : public Device {
 public:
  ParseResult ParseReportDescriptor(const hid::ReportDescriptor& hid_report_descriptor) override;
  ReportDescriptor GetDescriptor() override;

  ParseResult ParseReport(const uint8_t* data, size_t len, Report* report) override;

  uint8_t ReportId() const override { return report_id_; }

 private:
  hid::Attributes values_[kSensorMaxValues] = {};
  size_t num_values_ = 0;

  SensorDescriptor descriptor_ = {};

  size_t report_size_ = 0;
  uint8_t report_id_ = 0;
};

}  // namespace hid_input_report

#endif  // HID_INPUT_REPORT_SENSOR_H_
