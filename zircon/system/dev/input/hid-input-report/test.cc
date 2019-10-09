// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-loop/cpp/loop.h>
#include <lib/async/cpp/task.h>
#include <lib/fake_ddk/fake_ddk.h>
#include <lib/fidl-async/cpp/bind.h>
#include <lib/zx/channel.h>
#include <lib/zx/eventpair.h>
#include <unistd.h>

#include <ddktl/protocol/hiddevice.h>
#include <hid-input-report/descriptors.h>
#include <zxtest/zxtest.h>

#include "input-report.h"

namespace hid_input_report_dev {

struct ProtocolDeviceOps {
  const zx_protocol_device_t* ops;
  void* ctx;
};

// Create our own Fake Ddk Bind class. We want to save the last device arguments that
// have been seen, so the test can get ahold of the instance device and test
// Reads and Writes on it.
class Binder : public fake_ddk::Bind {
 public:
  zx_status_t DeviceAdd(zx_driver_t* drv, zx_device_t* parent, device_add_args_t* args,
                        zx_device_t** out) override {
    zx_status_t status;

    if (args && args->ops) {
      if (args->ops->message) {
        if ((status = fidl_.SetMessageOp(args->ctx, args->ops->message)) < 0) {
          return status;
        }
      }
    }

    *out = fake_ddk::kFakeDevice;
    add_called_ = true;

    last_ops_.ctx = args->ctx;
    last_ops_.ops = args->ops;

    return ZX_OK;
  }

  ProtocolDeviceOps GetLastDeviceOps() { return last_ops_; }

 private:
  ProtocolDeviceOps last_ops_;
};

const uint8_t boot_mouse_desc[] = {
    0x05, 0x01,  // Usage Page (Generic Desktop Ctrls)
    0x09, 0x02,  // Usage (Mouse)
    0xA1, 0x01,  // Collection (Application)
    0x09, 0x01,  //   Usage (Pointer)
    0xA1, 0x00,  //   Collection (Physical)
    0x05, 0x09,  //     Usage Page (Button)
    0x19, 0x01,  //     Usage Minimum (0x01)
    0x29, 0x03,  //     Usage Maximum (0x03)
    0x15, 0x00,  //     Logical Minimum (0)
    0x25, 0x01,  //     Logical Maximum (1)
    0x95, 0x03,  //     Report Count (3)
    0x75, 0x01,  //     Report Size (1)
    0x81, 0x02,  //     Input (Data,Var,Abs,No Wrap,Linear,No Null Position)
    0x95, 0x01,  //     Report Count (1)
    0x75, 0x05,  //     Report Size (5)
    0x81, 0x03,  //     Input (Const,Var,Abs,No Wrap,Linear,No Null Position
    0x05, 0x01,  //     Usage Page (Generic Desktop Ctrls)
    0x09, 0x30,  //     Usage (X)
    0x09, 0x31,  //     Usage (Y)
    0x15, 0x81,  //     Logical Minimum (-127)
    0x25, 0x7F,  //     Logical Maximum (127)
    0x75, 0x08,  //     Report Size (8)
    0x95, 0x02,  //     Report Count (2)
    0x81, 0x06,  //     Input (Data,Var,Rel,No Wrap,Linear,No Null Position)
    0xC0,        //   End Collection
    0xC0,        // End Collection
};

class FakeHidDevice : public ddk::HidDeviceProtocol<FakeHidDevice> {
 public:
  FakeHidDevice() : proto_({&hid_device_protocol_ops_, this}) {}

  zx_status_t HidDeviceRegisterListener(const hid_report_listener_protocol_t* listener) {
    listener_ = *listener;
    return ZX_OK;
  }

  void HidDeviceUnregisterListener() {}

  zx_status_t HidDeviceGetDescriptor(uint8_t* out_descriptor_list, size_t descriptor_count,
                                     size_t* out_descriptor_actual) {
    if (descriptor_count < report_desc_.size()) {
      return ZX_ERR_BUFFER_TOO_SMALL;
    }
    memcpy(out_descriptor_list, report_desc_.data(), report_desc_.size());
    *out_descriptor_actual = report_desc_.size();
    return ZX_OK;
  }

  zx_status_t HidDeviceGetReport(hid_report_type_t rpt_type, uint8_t rpt_id,
                                 uint8_t* out_report_list, size_t report_count,
                                 size_t* out_report_actual) {
    return ZX_OK;
  }

  zx_status_t HidDeviceSetReport(hid_report_type_t rpt_type, uint8_t rpt_id,
                                 const uint8_t* report_list, size_t report_count) {
    return ZX_OK;
  }

  void SetReportDesc(std::vector<uint8_t> report_desc) { report_desc_ = report_desc; }
  void SetReport(std::vector<uint8_t> report) { report_ = report; }

  void SendReport() {
    listener_.ops->receive_report(listener_.ctx, report_.data(), report_.size());
  }

  hid_report_listener_protocol_t listener_;
  hid_device_protocol_t proto_;
  std::vector<uint8_t> report_desc_;
  std::vector<uint8_t> report_;
};

class HidDevTest : public zxtest::Test {
  void SetUp() override {
    client_ = ddk::HidDeviceProtocolClient(&fake_hid_.proto_);
    device_ = new InputReport(fake_ddk::kFakeParent, client_);
    // Each test is responsible for calling |device_->Bind()|.
  }

  void TearDown() override {
    device_->DdkAsyncRemove();
    EXPECT_TRUE(ddk_.Ok());

    // This should delete the object, which means this test should not leak.
    device_->DdkRelease();
  }

 protected:
  Binder ddk_;
  FakeHidDevice fake_hid_;
  InputReport* device_;
  ddk::HidDeviceProtocolClient client_;
};

TEST_F(HidDevTest, HidLifetimeTest) {
  std::vector<uint8_t> boot_mouse(boot_mouse_desc, boot_mouse_desc + sizeof(boot_mouse_desc));
  fake_hid_.SetReportDesc(boot_mouse);

  ASSERT_OK(device_->Bind());
}

TEST_F(HidDevTest, InstanceLifetimeTest) {
  std::vector<uint8_t> boot_mouse(boot_mouse_desc, boot_mouse_desc + sizeof(boot_mouse_desc));
  fake_hid_.SetReportDesc(boot_mouse);

  ASSERT_OK(device_->Bind());

  // Open an instance device.
  zx_device_t* open_dev;
  ASSERT_OK(device_->DdkOpen(&open_dev, 0));
  ProtocolDeviceOps dev_ops = ddk_.GetLastDeviceOps();
  // Close the instance device.
  dev_ops.ops->close(dev_ops.ctx, 0);
}

TEST_F(HidDevTest, GetReportDescTest) {
  std::vector<uint8_t> boot_mouse(boot_mouse_desc, boot_mouse_desc + sizeof(boot_mouse_desc));
  fake_hid_.SetReportDesc(boot_mouse);

  ASSERT_OK(device_->Bind());

  // Open an instance device.
  zx_device_t* open_dev;
  ASSERT_OK(device_->DdkOpen(&open_dev, 0));
  // Opening the device created an instance device to be created, and we can
  // get its arguments here.
  ProtocolDeviceOps dev_ops = ddk_.GetLastDeviceOps();

  auto sync_client = llcpp_report::InputDevice::SyncClient(std::move(ddk_.FidlClient()));
  llcpp_report::InputDevice::ResultOf::GetDescriptor result = sync_client.GetDescriptor();
  ASSERT_OK(result.status());

  auto& desc = result.Unwrap()->descriptor;
  ASSERT_TRUE(desc.has_mouse());
  llcpp_report::MouseDescriptor& mouse = desc.mouse();

  ASSERT_TRUE(mouse.has_movement_x());
  ASSERT_EQ(-127, mouse.movement_x().range.min);
  ASSERT_EQ(127, mouse.movement_x().range.max);

  ASSERT_TRUE(mouse.has_movement_y());
  ASSERT_EQ(-127, mouse.movement_y().range.min);
  ASSERT_EQ(127, mouse.movement_y().range.max);

  // Close the instance device.
  dev_ops.ops->close(dev_ops.ctx, 0);
}

TEST_F(HidDevTest, GetReportTest) {
  std::vector<uint8_t> boot_mouse(boot_mouse_desc, boot_mouse_desc + sizeof(boot_mouse_desc));
  fake_hid_.SetReportDesc(boot_mouse);

  device_->Bind();

  // Open an instance device.
  zx_device_t* open_dev;
  ASSERT_OK(device_->DdkOpen(&open_dev, 0));
  // Opening the device created an instance device to be created, and we can
  // get its arguments here.
  ProtocolDeviceOps dev_ops = ddk_.GetLastDeviceOps();

  auto sync_client = llcpp_report::InputDevice::SyncClient(std::move(ddk_.FidlClient()));

  // Spoof send a report.
  std::vector<uint8_t> sent_report = {0xFF, 0x50, 0x70};
  fake_hid_.SetReport(sent_report);
  fake_hid_.SendReport();

  // Get the report.
  llcpp_report::InputDevice::ResultOf::GetReports result = sync_client.GetReports();
  ASSERT_OK(result.status());
  auto& reports = result.Unwrap()->reports;

  ASSERT_EQ(1, reports.count());

  auto& report = reports[0];
  ASSERT_TRUE(report.has_mouse());
  auto& mouse = report.mouse();

  ASSERT_TRUE(mouse.has_movement_x());
  ASSERT_EQ(0x50, mouse.movement_x());

  ASSERT_TRUE(mouse.has_movement_y());
  ASSERT_EQ(0x70, mouse.movement_y());

  ASSERT_TRUE(mouse.has_pressed_buttons());
  fidl::VectorView<uint8_t> pressed_buttons = mouse.pressed_buttons();
  for (size_t i = 0; i < pressed_buttons.count(); i++) {
    ASSERT_EQ(i + 1, pressed_buttons[i]);
  }

  // Close the instance device.
  dev_ops.ops->close(dev_ops.ctx, 0);
}

}  // namespace hid_input_report_dev
