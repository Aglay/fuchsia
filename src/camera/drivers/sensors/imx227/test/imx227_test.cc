// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/camera/drivers/sensors/imx227/imx227.h"

#include <endian.h>
#include <lib/fake_ddk/fake_ddk.h>
#include <lib/mock-i2c/mock-i2c.h>

#include <mock/ddktl/protocol/clock.h>
#include <mock/ddktl/protocol/gpio.h>
#include <mock/ddktl/protocol/mipicsi.h>
#include <zxtest/zxtest.h>

// The following equality operators are necessary for mocks.

bool operator==(const i2c_op_t& lhs, const i2c_op_t& rhs) { return true; }

bool operator==(const resolution_t& lhs, const resolution_t& rhs) {
  return lhs.width == rhs.width && lhs.height == rhs.height;
}

bool operator==(const mipi_adap_info_t& lhs, const mipi_adap_info_t& rhs) {
  return lhs.resolution == rhs.resolution && lhs.format == rhs.format && lhs.mode == rhs.mode &&
         lhs.path == rhs.path;
}

bool operator==(const mipi_info_t& lhs, const mipi_info_t& rhs) {
  return lhs.channel == rhs.channel && lhs.lanes == rhs.lanes && lhs.ui_value == rhs.ui_value &&
         lhs.csi_version == rhs.csi_version;
}

namespace camera {
namespace {

std::vector<uint8_t> SplitBytes(uint16_t bytes) {
  return std::vector<uint8_t>{static_cast<uint8_t>(bytes >> 8), static_cast<uint8_t>(bytes & 0xff)};
}

class FakeImx227Device : public Imx227Device {
 public:
  FakeImx227Device()
      : Imx227Device(fake_ddk::FakeParent(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr),
        proto_({&camera_sensor2_protocol_ops_, this}) {
    SetProtocols();
    ExpectInitPdev();
    ASSERT_OK(InitPdev());
    ASSERT_NO_FATAL_FAILURES(VerifyAll());
  }

  void ExpectInitPdev() {
    mock_gpio_cam_rst_.ExpectConfigOut(ZX_OK, 1);
    mock_gpio_vana_enable_.ExpectConfigOut(ZX_OK, 0);
    mock_gpio_vdig_enable_.ExpectConfigOut(ZX_OK, 0);
  }

  void ExpectInit() {
    mock_gpio_vana_enable_.ExpectWrite(ZX_OK, true);
    mock_gpio_vdig_enable_.ExpectWrite(ZX_OK, true);
    mock_clk24_.ExpectEnable(ZX_OK);
    mock_gpio_cam_rst_.ExpectWrite(ZX_OK, false);
  }

  void ExpectDeInit() {
    mock_mipi_.ExpectDeInit(ZX_OK);
    mock_gpio_cam_rst_.ExpectWrite(ZX_OK, true);
    mock_clk24_.ExpectDisable(ZX_OK);
    mock_gpio_vdig_enable_.ExpectWrite(ZX_OK, false);
    mock_gpio_vana_enable_.ExpectWrite(ZX_OK, false);
  }

  void ExpectGetSensorId() {
    const auto kSensorModelIdHiRegByteVec = SplitBytes(htobe16(kSensorModelIdReg));
    const auto kSensorModelIdLoRegByteVec = SplitBytes(htobe16(kSensorModelIdReg + 1));
    const auto kSensorModelIdDefaultByteVec = SplitBytes(kSensorModelIdDefault);
    // An I2C bus read is a write of the address followed by a read of the data.
    // In this case, there are two 8-bit reads occuring to get the full 16-bit Sensor Model ID.
    mock_i2c_.ExpectWrite({kSensorModelIdHiRegByteVec[1], kSensorModelIdHiRegByteVec[0]})
        .ExpectReadStop({kSensorModelIdDefaultByteVec[0]})
        .ExpectWrite({kSensorModelIdLoRegByteVec[1], kSensorModelIdLoRegByteVec[0]})
        .ExpectReadStop({kSensorModelIdDefaultByteVec[1]});
  }

  void SetProtocols() {
    i2c_ = ddk::I2cChannel(mock_i2c_.GetProto());
    gpio_vana_enable_ = ddk::GpioProtocolClient(mock_gpio_vana_enable_.GetProto());
    gpio_vdig_enable_ = ddk::GpioProtocolClient(mock_gpio_vdig_enable_.GetProto());
    gpio_cam_rst_ = ddk::GpioProtocolClient(mock_gpio_cam_rst_.GetProto());
    clk24_ = ddk::ClockProtocolClient(mock_clk24_.GetProto());
    mipi_ = ddk::MipiCsiProtocolClient(mock_mipi_.GetProto());
  }

  void VerifyAll() {
    mock_i2c_.VerifyAndClear();
    mock_gpio_vana_enable_.VerifyAndClear();
    mock_gpio_vdig_enable_.VerifyAndClear();
    mock_gpio_cam_rst_.VerifyAndClear();
    mock_clk24_.VerifyAndClear();
    mock_mipi_.VerifyAndClear();
  }

  const camera_sensor2_protocol_t* proto() const { return &proto_; }

 private:
  camera_sensor2_protocol_t proto_;
  mock_i2c::MockI2c mock_i2c_;
  ddk::MockGpio mock_gpio_vana_enable_;
  ddk::MockGpio mock_gpio_vdig_enable_;
  ddk::MockGpio mock_gpio_cam_rst_;
  ddk::MockClock mock_clk24_;
  ddk::MockMipiCsi mock_mipi_;
};

class Imx227DeviceTest : public zxtest::Test {
 public:
  Imx227DeviceTest() {
    fbl::Array<fake_ddk::ProtocolEntry> protocols(new fake_ddk::ProtocolEntry[1], 1);
    protocols[0] = {ZX_PROTOCOL_CAMERA_SENSOR2,
                    *reinterpret_cast<const fake_ddk::Protocol*>(dut_.proto())};
    ddk_.SetProtocols(std::move(protocols));
  }

  void SetUp() override {
    auto dut = std::make_unique<FakeImx227Device>();
    dut_.ExpectInit();
    dut_.ExpectDeInit();
  }

  void TearDown() override {
    dut().CameraSensor2DeInit();
    ASSERT_NO_FATAL_FAILURES(dut().VerifyAll());
  }

  FakeImx227Device& dut() { return dut_; }

 private:
  fake_ddk::Bind ddk_;
  FakeImx227Device dut_;
};

TEST_F(Imx227DeviceTest, Sanity) { ASSERT_OK(dut().CameraSensor2Init()); }

// TODO(50737): The expected I2C operations don't match up with those made by
// CameraSensor2GetSensorId.
TEST_F(Imx227DeviceTest, DISABLED_GetSensorId) {
  dut().ExpectGetSensorId();
  ASSERT_OK(dut().CameraSensor2Init());
  ASSERT_OK(dut().CameraSensor2GetSensorId(nullptr));
}

}  // namespace
}  // namespace camera
