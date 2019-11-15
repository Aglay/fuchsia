// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/lib/hid-input-report/touch.h"

#include <stdint.h>

#include <hid-parser/parser.h>
#include <hid-parser/report.h>
#include <hid-parser/units.h>
#include <hid-parser/usages.h>

#include "src/ui/lib/hid-input-report/device.h"

namespace hid_input_report {

ParseResult Touch::ParseReportDescriptor(const hid::ReportDescriptor& hid_report_descriptor) {
  ContactConfig contacts[kTouchMaxContacts];
  size_t num_contacts = 0;
  TouchDescriptor descriptor = {};

  // Traverse up the nested collections to the Application collection.
  hid::Collection* main_collection = hid_report_descriptor.input_fields[0].col;
  while (main_collection != nullptr) {
    if (main_collection->type == hid::CollectionType::kApplication) {
      break;
    }
    main_collection = main_collection->parent;
  }
  if (!main_collection) {
    return kParseNoCollection;
  }

  if (main_collection->usage ==
      hid::USAGE(hid::usage::Page::kDigitizer, hid::usage::Digitizer::kTouchScreen)) {
    descriptor.touch_type = ::llcpp::fuchsia::input::report::TouchType::TOUCHSCREEN;
  } else {
    return ParseResult::kParseNoCollection;
  }

  hid::Collection* finger_collection = nullptr;

  for (size_t i = 0; i < hid_report_descriptor.input_count; i++) {
    const hid::ReportField field = hid_report_descriptor.input_fields[i];

    // Process touch points. Don't process the item if it's not part of a touch point collection.
    if (field.col->usage !=
        hid::USAGE(hid::usage::Page::kDigitizer, hid::usage::Digitizer::kFinger)) {
      continue;
    }

    // If our collection pointer is different than the previous collection
    // pointer, we have started a new collection and are on a new touch point
    if (field.col != finger_collection) {
      finger_collection = field.col;
      num_contacts++;
    }

    if (num_contacts < 1) {
      return ParseResult::kParseNoCollection;
    }
    if (num_contacts > kTouchMaxContacts) {
      return kParseTooManyItems;
    }
    ContactConfig* contact = &contacts[num_contacts - 1];

    if (field.attr.usage ==
        hid::USAGE(hid::usage::Page::kDigitizer, hid::usage::Digitizer::kContactID)) {
      contact->contact_id = field.attr;
      SetAxisFromAttribute(contact->contact_id, &descriptor.contacts[num_contacts - 1].contact_id);
    }
    if (field.attr.usage ==
        hid::USAGE(hid::usage::Page::kDigitizer, hid::usage::Digitizer::kTipSwitch)) {
      contact->tip_switch = field.attr;
      SetAxisFromAttribute(contact->tip_switch, &descriptor.contacts[num_contacts - 1].is_pressed);
    }
    if (field.attr.usage ==
        hid::USAGE(hid::usage::Page::kGenericDesktop, hid::usage::GenericDesktop::kX)) {
      contact->position_x = field.attr;
      SetAxisFromAttribute(contact->position_x, &descriptor.contacts[num_contacts - 1].position_x);
    }
    if (field.attr.usage ==
        hid::USAGE(hid::usage::Page::kGenericDesktop, hid::usage::GenericDesktop::kY)) {
      contact->position_y = field.attr;
      SetAxisFromAttribute(contact->position_y, &descriptor.contacts[num_contacts - 1].position_y);
    }
    if (field.attr.usage ==
        hid::USAGE(hid::usage::Page::kDigitizer, hid::usage::Digitizer::kTipPressure)) {
      contact->pressure = field.attr;
      SetAxisFromAttribute(contact->pressure, &descriptor.contacts[num_contacts - 1].pressure);
    }
    if (field.attr.usage ==
        hid::USAGE(hid::usage::Page::kDigitizer, hid::usage::Digitizer::kWidth)) {
      contact->contact_width = field.attr;
      SetAxisFromAttribute(contact->contact_width,
                           &descriptor.contacts[num_contacts - 1].contact_width);
    }
    if (field.attr.usage ==
        hid::USAGE(hid::usage::Page::kDigitizer, hid::usage::Digitizer::kHeight)) {
      contact->contact_height = field.attr;
      SetAxisFromAttribute(contact->contact_height,
                           &descriptor.contacts[num_contacts - 1].contact_height);
    }
  }

  // No error, write to class members.
  for (size_t i = 0; i < num_contacts; i++) {
    contacts_[i] = contacts[i];
  }

  descriptor.max_contacts = static_cast<uint32_t>(num_contacts);
  descriptor.num_contacts = num_contacts;
  descriptor_ = descriptor;

  report_size_ = hid_report_descriptor.input_byte_sz;
  report_id_ = hid_report_descriptor.report_id;

  return kParseOk;
}

ReportDescriptor Touch::GetDescriptor() {
  ReportDescriptor report_descriptor = {};
  report_descriptor.descriptor = descriptor_;
  return report_descriptor;
}

ParseResult Touch::ParseReport(const uint8_t* data, size_t len, Report* report) {
  TouchReport touch_report = {};
  if (len != report_size_) {
    return kParseReportSizeMismatch;
  }

  double value_out;

  // Extract each touch item.
  size_t contact_num = 0;
  for (size_t i = 0; i < descriptor_.num_contacts; i++) {
    ContactReport& contact = touch_report.contacts[contact_num];
    if (descriptor_.contacts[i].is_pressed.enabled) {
      if (hid::ExtractAsUnitType(data, len, contacts_[i].tip_switch, &value_out)) {
        contact.is_pressed = static_cast<bool>(value_out);
        contact.has_is_pressed = true;
        if (!contact.is_pressed) {
          continue;
        }
      }
    }
    contact_num++;
    if (descriptor_.contacts[i].contact_id.enabled) {
      // Some touchscreens we support mistakenly set the logical range to 0-1 for the
      // tip switch and then never reset the range for the contact id. For this reason,
      // we have to do an "unconverted" extraction.
      if (hid::ExtractUint(data, len, contacts_[i].contact_id, &contact.contact_id)) {
        contact.has_contact_id = true;
      }
    }
    if (descriptor_.contacts[i].position_x.enabled) {
      if (hid::ExtractAsUnitType(data, len, contacts_[i].position_x, &value_out)) {
        contact.position_x = static_cast<int64_t>(value_out);
        contact.has_position_x = true;
      }
    }
    if (descriptor_.contacts[i].position_y.enabled) {
      if (hid::ExtractAsUnitType(data, len, contacts_[i].position_y, &value_out)) {
        contact.position_y = static_cast<int64_t>(value_out);
        contact.has_position_y = true;
      }
    }
    if (descriptor_.contacts[i].pressure.enabled) {
      if (hid::ExtractAsUnitType(data, len, contacts_[i].pressure, &value_out)) {
        contact.pressure = static_cast<int64_t>(value_out);
        contact.has_pressure = true;
      }
    }
    if (descriptor_.contacts[i].contact_width.enabled) {
      if (hid::ExtractAsUnitType(data, len, contacts_[i].contact_width, &value_out)) {
        contact.contact_width = static_cast<int64_t>(value_out);
        contact.has_contact_width = true;
      }
    }
    if (descriptor_.contacts[i].contact_height.enabled) {
      if (hid::ExtractAsUnitType(data, len, contacts_[i].contact_height, &value_out)) {
        contact.contact_height = static_cast<int64_t>(value_out);
        contact.has_contact_height = true;
      }
    }
  }
  touch_report.num_contacts = contact_num;

  // Now that we can't fail, set the real report.
  report->report = touch_report;

  return kParseOk;
}

}  // namespace hid_input_report
