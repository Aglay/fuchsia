// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_INPUT_DRIVERS_HID_INPUT_REPORT_INPUT_REPORT_H_
#define SRC_UI_INPUT_DRIVERS_HID_INPUT_REPORT_INPUT_REPORT_H_

#include <vector>

#include <ddktl/device.h>
#include <ddktl/protocol/hiddevice.h>
#include <fbl/intrusive_double_list.h>
#include <fbl/mutex.h>

#include "instance.h"
#include "src/ui/input/lib/hid-input-report/device.h"

namespace hid_input_report_dev {

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

  void HidReportListenerReceiveReport(const uint8_t* report, size_t report_size,
                                      zx_time_t report_time);

  void RemoveInstanceFromList(InputReportInstance* instance) override;
  void CreateDescriptor(fidl::Allocator* allocator,
                        fuchsia_input_report::DeviceDescriptor::Builder* descriptor) override;
  zx_status_t SendOutputReport(fuchsia_input_report::OutputReport report) override;

 private:
  bool ParseHidInputReportDescriptor(const hid::ReportDescriptor* report_desc);

  ddk::HidDeviceProtocolClient hiddev_;

  fbl::Mutex instance_lock_;
  uint32_t next_instance_id_ = 0;
  // Unmanaged linked-list because the HidInstances free themselves through DdkRelease.
  fbl::DoublyLinkedList<InputReportInstance*> instance_list_ __TA_GUARDED(instance_lock_);

  std::vector<std::unique_ptr<hid_input_report::Device>> devices_;
};

}  // namespace hid_input_report_dev

#endif  // SRC_UI_INPUT_DRIVERS_HID_INPUT_REPORT_INPUT_REPORT_H_
