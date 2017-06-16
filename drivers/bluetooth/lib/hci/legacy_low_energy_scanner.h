// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <memory>
#include <unordered_map>

#include "apps/bluetooth/lib/common/byte_buffer.h"
#include "apps/bluetooth/lib/hci/command_channel.h"
#include "apps/bluetooth/lib/hci/hci.h"
#include "apps/bluetooth/lib/hci/low_energy_scanner.h"
#include "lib/ftl/functional/cancelable_callback.h"
#include "lib/ftl/macros.h"
#include "lib/ftl/memory/ref_counted.h"
#include "lib/ftl/tasks/task_runner.h"

namespace bluetooth {
namespace hci {

// LegacyLowEnergyScanner implements the LowEnergyScanner interface for controllers that
// do not support the 5.0 Extended Advertising feature. This uses the legacy HCI LE device scan
// commands and events:
//     - HCI_LE_Set_Scan_Parameters
//     - HCI_LE_Set_Scan_Enable
//     - HCI_LE_Advertising_Report event
class LegacyLowEnergyScanner : public LowEnergyScanner {
 public:
  LegacyLowEnergyScanner(Delegate* delegate, ftl::RefPtr<Transport> hci,
                         ftl::RefPtr<ftl::TaskRunner> task_runner);
  ~LegacyLowEnergyScanner() override;

  // LowEnergyScanner overrides:
  bool StartScan(bool active, uint16_t scan_interval, uint16_t scan_window, bool filter_duplicates,
                 hci::LEScanFilterPolicy filter_policy, int64_t period_ms,
                 const StatusCallback& callback) override;
  bool StopScan() override;

 private:
  struct PendingScanResult {
    explicit PendingScanResult(const common::DeviceAddress& address);

    LowEnergyScanResult result;

    // Make this large enough to store both advertising and scan response data PDUs.
    size_t adv_data_len;
    common::StaticByteBuffer<hci::kMaxLEAdvertisingDataLength * 2> data;
  };

  // Called by StopScan() and by the scan timeout handler set up by StartScan().
  void StopScanInternal(bool stopped);

  // Event handler for HCI LE Advertising Report event.
  void OnAdvertisingReportEvent(const hci::EventPacket& event);

  // Called when a Scan Response is received during an active scan.
  void HandleScanResponse(const hci::LEAdvertisingReportData& report, int8_t rssi);

  // Notifies observers of a device that was found.
  void NotifyDeviceFound(const LowEnergyScanResult& result, const common::ByteBuffer& data);

  // True if an active scan is currenty being performed. False, if passive.
  bool active_scanning_;

  // Callback passed in to the most recently accepted call to StartScan();
  StatusCallback scan_cb_;

  // The scan period timeout handler for the currently active scan session.
  ftl::CancelableClosure scan_timeout_cb_;

  // Our event handler ID for the LE Advertising Report event.
  hci::CommandChannel::EventHandlerId event_handler_id_;

  // Scannable advertising events for which a Scan Response PDU has not been received. This is
  // accumulated during a discovery procedure and always cleared at the end of the scan period.
  std::unordered_map<common::DeviceAddress, PendingScanResult> pending_results_;

  FTL_DISALLOW_COPY_AND_ASSIGN(LegacyLowEnergyScanner);
};

}  // namespace hci
}  // namespace bluetooth
