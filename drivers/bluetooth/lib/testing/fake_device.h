// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include "apps/bluetooth/lib/common/byte_buffer.h"
#include "apps/bluetooth/lib/common/device_address.h"
#include "apps/bluetooth/lib/hci/hci.h"
#include "lib/ftl/macros.h"

namespace bluetooth {
namespace testing {

// FakeDevice is used to emulate remote Bluetooth devices.
class FakeDevice {
 public:
  FakeDevice(const common::DeviceAddress& address, bool connectable, bool scannable);

  void SetAdvertisingData(const common::ByteBuffer& data);

  // |should_batch_reports| indicates to the FakeController that the SCAN_IND report should be
  // included in the same HCI LE Advertising Report Event payload that includes the original
  // advertising data (see comments for should_batch_reports()).
  void SetScanResponse(bool should_batch_reports, const common::ByteBuffer& data);

  // Indicates whether or not this device should include the scan response and the advertising data
  // in the same HCI LE Advertising Report Event. This is used to test that the host stack can
  // correctly consolidate advertising reports when the payloads are spread across events and when
  // they are batched together in the same event.
  //
  // This isn't used by FakeDevice directly to generated batched reports. Rather it is a hint to the
  // corresponding FakeController which decides how the reports should be generated.
  bool should_batch_reports() const { return should_batch_reports_; }

  bool scannable() const { return scannable_; }

  // Generates and returns a LE Advertising Report Event payload. If |include_scan_rsp| is true,
  // then the returned PDU will contain two reports including the SCAN_IND report.
  common::DynamicByteBuffer CreateAdvertisingReportEvent(bool include_scan_rsp) const;

  // Generates a LE Advertising Report Event payload containing the scan response.
  common::DynamicByteBuffer CreateScanResponseReportEvent() const;

 private:
  void WriteScanResponseReport(hci::LEAdvertisingReportData* report) const;

  common::DeviceAddress address_;
  bool connectable_;
  bool scannable_;

  bool should_batch_reports_;
  common::DynamicByteBuffer adv_data_;
  common::DynamicByteBuffer scan_rsp_;

  FTL_DISALLOW_COPY_AND_ASSIGN(FakeDevice);
};

}  // namespace testing
}  // namespace bluetooth
