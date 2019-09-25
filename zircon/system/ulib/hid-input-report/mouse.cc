// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdint.h>

#include <hid-input-report/device.h>
#include <hid-input-report/mouse.h>
#include <hid-parser/parser.h>
#include <hid-parser/report.h>
#include <hid-parser/units.h>
#include <hid-parser/usages.h>

namespace hid_input_report {

void SetAxisFromAttribute(const hid::Attributes& attrs, Axis* axis) {
  axis->enabled = true;
  axis->range.min = static_cast<int64_t>(
      hid::unit::ConvertValToUnitType(attrs.unit, static_cast<double>(attrs.phys_mm.min)));
  axis->range.max = static_cast<int64_t>(
      hid::unit::ConvertValToUnitType(attrs.unit, static_cast<double>(attrs.phys_mm.max)));
  axis->unit = hid::unit::GetUnitTypeFromUnit(attrs.unit);
}

ParseResult Mouse::ParseReportDescriptor(const hid::ReportDescriptor& hid_report_descriptor) {
  hid::Attributes movement_x = {};
  hid::Attributes movement_y = {};
  hid::Attributes buttons[kMouseMaxButtons];
  uint8_t num_buttons = 0;

  MouseDescriptor mouse_descriptor = {};

  for (size_t i = 0; i < hid_report_descriptor.input_count; i++) {
    const hid::ReportField& field = hid_report_descriptor.input_fields[i];

    if (field.attr.usage ==
        hid::USAGE(hid::usage::Page::kGenericDesktop, hid::usage::GenericDesktop::kX)) {
      movement_x = field.attr;
      SetAxisFromAttribute(movement_x, &mouse_descriptor.movement_x);

    } else if (field.attr.usage ==
               hid::USAGE(hid::usage::Page::kGenericDesktop, hid::usage::GenericDesktop::kY)) {
      movement_y = field.attr;
      SetAxisFromAttribute(movement_y, &mouse_descriptor.movement_y);
    } else if (field.attr.usage.page == hid::usage::Page::kButton) {
      if (num_buttons == kMouseMaxButtons) {
        return kParseTooManyItems;
      }
      buttons[num_buttons++] = field.attr;
      mouse_descriptor.button_ids[num_buttons] = static_cast<uint8_t>(field.attr.usage.usage);
      mouse_descriptor.num_buttons++;
    }
  }

  // No error, write to class members.
  movement_x_ = movement_x;
  movement_y_ = movement_y;
  for (size_t i = 0; i < num_buttons; i++) {
    buttons_[i] = buttons[i];
  }
  num_buttons_ = num_buttons;

  mouse_descriptor.num_buttons = num_buttons;
  descriptor_ = mouse_descriptor;

  report_size_ = hid_report_descriptor.input_byte_sz;
  report_id_ = hid_report_descriptor.report_id;

  return kParseOk;
}

ReportDescriptor Mouse::GetDescriptor() {
  ReportDescriptor report_descriptor = {};
  report_descriptor.descriptor = descriptor_;
  return report_descriptor;
}

ParseResult Mouse::ParseReport(const uint8_t* data, size_t len, Report* report) {
  MouseReport mouse_report = {};
  if (len != report_size_) {
    return kParseReportSizeMismatch;
  }

  if (descriptor_.movement_x.enabled) {
    double value_out;
    if (hid::ExtractAsUnitType(data, len, movement_x_, &value_out)) {
      mouse_report.movement_x = static_cast<int64_t>(value_out);
      mouse_report.has_movement_x = true;
    }
  }
  if (descriptor_.movement_y.enabled) {
    double value_out;
    if (hid::ExtractAsUnitType(data, len, movement_y_, &value_out)) {
      mouse_report.movement_y = static_cast<int64_t>(value_out);
      mouse_report.has_movement_y = true;
    }
  }
  for (size_t i = 0; i < num_buttons_; i++) {
    double value_out;
    if (hid::ExtractAsUnitType(data, len, buttons_[i], &value_out)) {
      uint8_t pressed = (value_out > 0) ? 1 : 0;
      if (pressed) {
        mouse_report.buttons_pressed[mouse_report.num_buttons_pressed] =
            static_cast<uint8_t>(buttons_[i].usage.usage);
        mouse_report.num_buttons_pressed++;
      }
    }
  }

  // Now that we can't fail, set the real report.
  report->report = mouse_report;

  return kParseOk;
}

}  // namespace hid_input_report
