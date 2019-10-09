// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZIRCON_SYSTEM_DEV_INPUT_HID_INPUT_REPORT_INPUT_REPORT_H_
#define ZIRCON_SYSTEM_DEV_INPUT_HID_INPUT_REPORT_INPUT_REPORT_H_

#include <vector>

#include <ddktl/device.h>
#include <ddktl/protocol/hiddevice.h>
#include <fbl/intrusive_double_list.h>
#include <fbl/mutex.h>
#include <hid-input-report/device.h>

#include "input-report-instance.h"

namespace hid_input_report_dev {

class InputReportBase {
 public:
  virtual void RemoveInstanceFromList(InputReportInstance* instance) = 0;
  virtual const hid_input_report::ReportDescriptor* GetDescriptors(size_t* size) = 0;
};

class InputReport;
using DeviceType = ddk::Device<InputReport, ddk::UnbindableNew, ddk::Openable>;
class InputReport : public DeviceType,
                    public InputReportBase,
                    ddk::HidReportListenerProtocol<InputReport>,
                    public ddk::EmptyProtocol<ZX_PROTOCOL_INPUTREPORT> {
 public:
  InputReport(zx_device_t* parent, ddk::HidDeviceProtocolClient hiddev)
      : DeviceType(parent), hiddev_(hiddev) {}
  virtual ~InputReport() = default;

  zx_status_t Bind();
  zx_status_t DdkOpen(zx_device_t** dev_out, uint32_t flags);
  void DdkUnbindNew(ddk::UnbindTxn txn);
  void DdkRelease() { delete this; }

  void HidReportListenerReceiveReport(const uint8_t* report, size_t report_size);

  void RemoveInstanceFromList(InputReportInstance* instance) override;
  const hid_input_report::ReportDescriptor* GetDescriptors(size_t* size) override;

 private:
  bool ParseHidInputReportDescriptor(const hid::ReportDescriptor* report_desc);

  ddk::HidDeviceProtocolClient hiddev_;

  fbl::Mutex instance_lock_;
  // Unmanaged linked-list because the HidInstances free themselves through DdkRelease.
  fbl::DoublyLinkedList<InputReportInstance*> instance_list_ __TA_GUARDED(instance_lock_);

  std::vector<hid_input_report::ReportDescriptor> descriptors_;
  std::vector<std::unique_ptr<hid_input_report::Device>> devices_;
};

}  // namespace hid_input_report_dev

#endif  // ZIRCON_SYSTEM_DEV_INPUT_HID_INPUT_REPORT_INPUT_REPORT_H_
