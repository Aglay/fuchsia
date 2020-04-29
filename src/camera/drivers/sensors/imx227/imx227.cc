// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "imx227.h"

#include <endian.h>
#include <lib/device-protocol/i2c.h>
#include <lib/driver-unit-test/utils.h>
#include <lib/fit/result.h>
#include <stdint.h>
#include <threads.h>
#include <zircon/types.h>

#include <memory>

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/metadata.h>
#include <ddk/metadata/camera.h>
#include <fbl/auto_call.h>
#include <fbl/auto_lock.h>
#include <hw/reg.h>

#include "src/camera/drivers/sensors/imx227/imx227_seq.h"

namespace camera {

namespace {

constexpr uint16_t kSensorModelIdReg = 0x0016;
constexpr uint16_t kModeSelectReg = 0x0100;
constexpr uint16_t kFrameLengthLinesReg = 0x0340;
constexpr uint16_t kLineLengthPckReg = 0x0342;
constexpr uint8_t kByteShift = 8;
constexpr uint8_t kRaw10Bits = 10;
constexpr uint8_t kRaw12Bits = 12;
constexpr uint8_t kByteMask = 0xFF;
constexpr uint16_t kSensorId = 0x0227;
constexpr uint32_t kAGainPrecision = 12;
constexpr uint32_t kDGainPrecision = 8;
constexpr int32_t kLog2GainShift = 18;
constexpr int32_t kSensorExpNumber = 1;
constexpr uint32_t kMasterClock = 288000000;
constexpr uint32_t kMaxIntegrationTime =
    0x15BC;  // Max allowed for 30fps = 2782 (dec)=0x0ADE (hex) 15fps = 5564 (dec)=0x15BC (hex).
constexpr uint16_t kEndOfSequence = 0x0000;
}  // namespace

// Gets the register value from the sequence table.
// |id| : Index of the sequence table.
// |address| : Address of the register.
static fit::result<uint8_t, zx_status_t> GetRegisterValueFromSequence(uint8_t index,
                                                                      uint16_t address) {
  if (index >= kSEQUENCE_TABLE.size()) {
    return fit::error(ZX_ERR_INVALID_ARGS);
  }
  const InitSeqFmt* sequence = kSEQUENCE_TABLE[index];
  while (true) {
    uint16_t register_address = sequence->address;
    uint16_t register_value = sequence->value;
    uint16_t register_len = sequence->len;
    if (register_address == kEndOfSequence && register_value == 0 && register_len == 0) {
      break;
    }
    if (address == register_address) {
      return fit::ok(register_value);
    }
    sequence++;
  }
  return fit::error(ZX_ERR_NOT_FOUND);
}

zx_status_t Imx227Device::InitPdev() {
  std::lock_guard guard(lock_);

  // I2c for communicating with the sensor.
  if (!i2c_.is_valid()) {
    zxlogf(ERROR, "%s; I2C not available", __func__);
    return ZX_ERR_NO_RESOURCES;
  }

  // Clk for gating clocks for sensor.
  if (!clk24_.is_valid()) {
    zxlogf(ERROR, "%s; clk24_ not available", __func__);
    return ZX_ERR_NO_RESOURCES;
  }

  // Mipi for init and de-init.
  if (!mipi_.is_valid()) {
    zxlogf(ERROR, "%s; mipi_ not available", __func__);
    return ZX_ERR_NO_RESOURCES;
  }

  // GPIOs
  if (!gpio_vana_enable_.is_valid()) {
    zxlogf(ERROR, "%s; gpio_vana_enable_ not available", __func__);
    return ZX_ERR_NO_RESOURCES;
  }

  if (!gpio_vdig_enable_.is_valid()) {
    zxlogf(ERROR, "%s; gpio_vdig_enable_ not available", __func__);
    return ZX_ERR_NO_RESOURCES;
  }

  if (!gpio_cam_rst_.is_valid()) {
    zxlogf(ERROR, "%s; gpio_cam_rst_ not available", __func__);
    return ZX_ERR_NO_RESOURCES;
  }

  // Set the GPIO to output and set them to their initial values
  // before the power up sequence.
  gpio_cam_rst_.ConfigOut(1);
  gpio_vana_enable_.ConfigOut(0);
  gpio_vdig_enable_.ConfigOut(0);
  return ZX_OK;
}

fit::result<uint16_t, zx_status_t> Imx227Device::Read16(uint16_t addr) {
  auto result = Read8(addr);
  if (result.is_error()) {
    return fit::error(result.error());
  }
  auto upper_byte = result.value();
  result = Read8(addr + 1);
  if (result.is_error()) {
    return fit::error(result.error());
  }
  auto lower_byte = result.value();
  uint16_t reg_value = upper_byte << kByteShift | lower_byte;
  return fit::ok(reg_value);
}

fit::result<uint8_t, zx_status_t> Imx227Device::Read8(uint16_t addr) {
  // Convert the address to Big Endian format.
  // The camera sensor expects in this format.
  uint16_t buf = htobe16(addr);
  uint8_t val = 0;
  auto status =
      i2c_.WriteReadSync(reinterpret_cast<uint8_t*>(&buf), sizeof(buf), &val, sizeof(val));
  if (status != ZX_OK) {
    zxlogf(ERROR, "Imx227Device: could not read reg addr: 0x%08x  status: %d", addr, status);
    return fit::error(status);
  }
  return fit::ok(val);
}

zx_status_t Imx227Device::Write8(uint16_t addr, uint8_t val) {
  // Convert the address to Big Endian format.
  // The camera sensor expects in this format.
  // First two bytes are the address, third one is the value to be written.
  std::array<uint8_t, 3> buf;
  buf[0] = static_cast<uint8_t>((addr >> kByteShift) & kByteMask);
  buf[1] = static_cast<uint8_t>(addr & kByteMask);
  buf[2] = val;

  auto status = i2c_.WriteSync(buf.data(), buf.size());
  if (status != ZX_OK) {
    zxlogf(ERROR,
           "Imx227Device: could not write reg addr/val: 0x%08x/0x%08x status: "
           "%d\n",
           addr, val, status);
  }
  return status;
}

bool Imx227Device::CameraSensorIsPoweredUp() {
  std::lock_guard guard(lock_);
  return ValidateSensorID();
}

bool Imx227Device::ValidateSensorID() {
  auto result = Read16(kSensorModelIdReg);
  if (result.is_error()) {
    return false;
  }
  return result.value() == kSensorId;
}

zx_status_t Imx227Device::InitSensor(uint8_t idx) {
  if (idx >= kSEQUENCE_TABLE.size()) {
    return ZX_ERR_INVALID_ARGS;
  }

  const InitSeqFmt* sequence = kSEQUENCE_TABLE[idx];
  bool init_command = true;

  while (init_command) {
    uint16_t address = sequence->address;
    uint8_t value = sequence->value;

    switch (address) {
      case 0x0000: {
        if (sequence->value == 0 && sequence->len == 0) {
          init_command = false;
        } else {
          Write8(address, value);
        }
        break;
      }
      default:
        Write8(address, value);
        break;
    }
    sequence++;
  }
  return ZX_OK;
}

void Imx227Device::HwInit() {
  // Power up sequence. Reference: Page 51- IMX227-0AQH5-C datasheet.
  gpio_vana_enable_.Write(1);
  zx_nanosleep(zx_deadline_after(ZX_MSEC(50)));

  gpio_vdig_enable_.Write(1);
  zx_nanosleep(zx_deadline_after(ZX_MSEC(50)));

  // Enable 24M clock for sensor.
  clk24_.Enable();
  zx_nanosleep(zx_deadline_after(ZX_MSEC(10)));

  gpio_cam_rst_.Write(0);
  zx_nanosleep(zx_deadline_after(ZX_MSEC(50)));
}

void Imx227Device::HwDeInit() {
  gpio_cam_rst_.Write(1);
  zx_nanosleep(zx_deadline_after(ZX_MSEC(50)));

  clk24_.Disable();
  zx_nanosleep(zx_deadline_after(ZX_MSEC(10)));

  gpio_vdig_enable_.Write(0);
  zx_nanosleep(zx_deadline_after(ZX_MSEC(50)));

  gpio_vana_enable_.Write(0);
  zx_nanosleep(zx_deadline_after(ZX_MSEC(50)));
}

zx_status_t Imx227Device::CameraSensorInit() {
  std::lock_guard guard(lock_);

  HwInit();

  // Initialize Sensor Context.
  ctx_.seq_width = 1;
  ctx_.streaming_flag = 0;
  ctx_.again_old = 0;
  ctx_.change_flag = 0;
  ctx_.again_limit = 8 << kAGainPrecision;
  ctx_.dgain_limit = 15 << kDGainPrecision;

  // Initialize Sensor Parameters.
  ctx_.param.again_accuracy = 1 << kLog2GainShift;
  ctx_.param.sensor_exp_number = kSensorExpNumber;
  ctx_.param.again_log2_max = 3 << kLog2GainShift;
  ctx_.param.dgain_log2_max = 3 << kLog2GainShift;
  ctx_.param.integration_time_apply_delay = 2;
  ctx_.param.isp_exposure_channel_delay = 0;

  initialized_ = true;
  zxlogf(TRACE, "%s IMX227 Camera Sensor Brought out of reset", __func__);
  return ZX_OK;
}

void Imx227Device::CameraSensorDeInit() {
  std::lock_guard guard(lock_);
  mipi_.DeInit();
  // Reference code has it, mostly likely needed for the clock to
  // stabalize. No other way of knowing for sure if sensor is now off.
  zx_nanosleep(zx_deadline_after(ZX_MSEC(10)));
  initialized_ = false;
}

zx_status_t Imx227Device::CameraSensorGetInfo(camera_sensor_info_t* out_info) {
  std::lock_guard guard(lock_);
  memcpy(out_info, &ctx_.param, sizeof(camera_sensor_info_t));
  return ZX_OK;
}

zx_status_t Imx227Device::CameraSensorGetSupportedModes(camera_sensor_mode_t* out_modes_list,
                                                        size_t modes_count,
                                                        size_t* out_modes_actual) {
  std::lock_guard guard(lock_);
  if (modes_count > supported_modes.size()) {
    return ZX_ERR_INVALID_ARGS;
  }

  memcpy(out_modes_list, &supported_modes, sizeof(camera_sensor_mode_t) * supported_modes.size());
  *out_modes_actual = supported_modes.size();
  return ZX_OK;
}

// TODO(braval): Update the Banjo documentation to indicate that SensorSetMode() will
//               return ZX_OK even when the sensor is powered down and not
//               initialized into the requested mode.
zx_status_t Imx227Device::CameraSensorSetMode(uint8_t mode) {
  std::lock_guard guard(lock_);
  zxlogf(TRACE, "%s IMX227 Camera Sensor Mode Set request to %d", __func__, mode);

  HwInit();

  if (!IsSensorInitialized()) {
    return ZX_ERR_INTERNAL;
  }

  if (mode >= supported_modes.size()) {
    return ZX_ERR_INVALID_ARGS;
  }

  switch (supported_modes[mode].wdr_mode) {
    case CAMERASENSOR_WDR_MODE_LINEAR: {
      if (IsSensorOutOfReset()) {
        InitSensor(supported_modes[mode].idx);
      }

      ctx_.again_delay = 0;
      ctx_.dgain_delay = 0;
      ctx_.param.integration_time_apply_delay = 2;
      ctx_.param.isp_exposure_channel_delay = 0;
      ctx_.hdr_flag = 0;
      break;
    }
    // TODO(41260) : Support other modes.
    default:
      return ZX_ERR_NOT_SUPPORTED;
  }

  ctx_.param.active.width = supported_modes[mode].resolution.width;
  ctx_.param.active.height = supported_modes[mode].resolution.height;
  auto hmax_result = GetRegisterValueFromSequence(supported_modes[mode].idx, kLineLengthPckReg);
  auto vmax_result = GetRegisterValueFromSequence(supported_modes[mode].idx, kFrameLengthLinesReg);
  if (hmax_result.is_error() || vmax_result.is_error()) {
    return ZX_ERR_INTERNAL;
  }
  ctx_.HMAX = hmax_result.value();
  ctx_.VMAX = vmax_result.value();
  ctx_.int_max = kMaxIntegrationTime;
  ctx_.int_time_min = 1;
  ctx_.int_time_limit = ctx_.int_max;
  ctx_.param.total.height = ctx_.VMAX;
  ctx_.param.total.width = ctx_.HMAX;
  ctx_.param.pixels_per_line = ctx_.param.total.width;

  uint32_t master_clock = kMasterClock;
  ctx_.param.lines_per_second = master_clock / ctx_.HMAX;

  ctx_.param.integration_time_min = ctx_.int_time_min;
  ctx_.param.integration_time_limit = ctx_.int_time_limit;
  ctx_.param.integration_time_max = ctx_.int_time_limit;
  ctx_.param.integration_time_long_max = ctx_.int_time_limit;
  ctx_.param.mode = mode;
  ctx_.param.bayer = supported_modes[mode].bayer;
  ctx_.wdr_mode = supported_modes[mode].wdr_mode;

  mipi_info_t mipi_info;
  mipi_adap_info_t adap_info;

  mipi_info.lanes = supported_modes[mode].lanes;
  mipi_info.ui_value = 1000 / supported_modes[mode].mbps;
  if ((1000 % supported_modes[mode].mbps) != 0) {
    mipi_info.ui_value += 1;
  }

  switch (supported_modes[mode].bits) {
    case kRaw10Bits:
      adap_info.format = MIPI_IMAGE_FORMAT_AM_RAW10;
      break;
    case kRaw12Bits:
      adap_info.format = MIPI_IMAGE_FORMAT_AM_RAW12;
      break;
    default:
      adap_info.format = MIPI_IMAGE_FORMAT_AM_RAW10;
  }

  adap_info.resolution.width = supported_modes[mode].resolution.width;
  adap_info.resolution.height = supported_modes[mode].resolution.height;
  adap_info.path = MIPI_PATH_PATH0;
  adap_info.mode = MIPI_MODES_DIR_MODE;
  auto status = mipi_.Init(&mipi_info, &adap_info);
  return status;
}

zx_status_t Imx227Device::CameraSensorStartStreaming() {
  std::lock_guard guard(lock_);
  if (!IsSensorInitialized() || ctx_.streaming_flag) {
    return ZX_ERR_BAD_STATE;
  }
  zxlogf(TRACE, "%s Camera Sensor Start Streaming", __func__);
  ctx_.streaming_flag = 1;
  Write8(kModeSelectReg, 0x01);
  return ZX_OK;
}

zx_status_t Imx227Device::CameraSensorStopStreaming() {
  std::lock_guard guard(lock_);
  if (!IsSensorInitialized() || !ctx_.streaming_flag) {
    return ZX_ERR_BAD_STATE;
  }
  ctx_.streaming_flag = 0;
  HwDeInit();
  return ZX_OK;
}

int32_t Imx227Device::CameraSensorSetAnalogGain(int32_t gain) {
  std::lock_guard guard(lock_);
  return ZX_ERR_NOT_SUPPORTED;
}

int32_t Imx227Device::CameraSensorSetDigitalGain(int32_t gain) {
  std::lock_guard guard(lock_);
  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t Imx227Device::CameraSensorSetIntegrationTime(int32_t int_time) {
  std::lock_guard guard(lock_);

  // TODO(41260): Add support for this.
  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t Imx227Device::CameraSensorUpdate() {
  std::lock_guard guard(lock_);
  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t Imx227Device::Create(zx_device_t* parent, std::unique_ptr<Imx227Device>* device_out) {
  ddk::CompositeProtocolClient composite(parent);
  if (!composite.is_valid()) {
    zxlogf(ERROR, "%s could not get composite protocoln", __func__);
    return ZX_ERR_NOT_SUPPORTED;
  }

  std::array<zx_device_t*, FRAGMENT_COUNT> fragments;
  size_t actual;
  composite.GetFragments(fragments.data(), FRAGMENT_COUNT, &actual);
  if (actual != FRAGMENT_COUNT) {
    zxlogf(ERROR, "%s Could not get fragments", __func__);
    return ZX_ERR_NOT_SUPPORTED;
  }

  auto sensor_device = std::make_unique<Imx227Device>(
      parent, fragments[FRAGMENT_I2C], fragments[FRAGMENT_GPIO_VANA], fragments[FRAGMENT_GPIO_VDIG],
      fragments[FRAGMENT_GPIO_CAM_RST], fragments[FRAGMENT_CLK24], fragments[FRAGMENT_MIPICSI]);

  zx_status_t status = sensor_device->InitPdev();
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s InitPdev failed", __func__);
    return status;
  }
  *device_out = std::move(sensor_device);
  return status;
}

void Imx227Device::ShutDown() {}

void Imx227Device::DdkUnbindNew(ddk::UnbindTxn txn) { txn.Reply(); }

void Imx227Device::DdkRelease() {
  ShutDown();
  delete this;
}

zx_status_t Imx227Device::CreateAndBind(void* /*ctx*/, zx_device_t* parent) {
  std::unique_ptr<Imx227Device> device;
  zx_status_t status = Imx227Device::Create(parent, &device);
  if (status != ZX_OK) {
    zxlogf(ERROR, "imx227: Could not setup imx227 sensor device: %d", status);
    return status;
  }
  std::array<zx_device_prop_t, 1> props = {{
      {BIND_PLATFORM_PROTO, 0, ZX_PROTOCOL_CAMERA_SENSOR},
  }};

  status = device->DdkAdd("imx227", DEVICE_ADD_ALLOW_MULTI_COMPOSITE, props.data(), props.size());
  if (status != ZX_OK) {
    zxlogf(ERROR, "imx227: Could not add imx227 sensor device: %d", status);
    return status;
  }
  zxlogf(INFO, "imx227 driver added");

  // `device` intentionally leaked as it is now held by DevMgr.
  __UNUSED auto* dev = device.release();
  return ZX_OK;
}

bool Imx227Device::RunUnitTests(void* ctx, zx_device_t* parent, zx_handle_t channel) {
  return driver_unit_test::RunZxTests("Imx227Tests", parent, channel);
}

static constexpr zx_driver_ops_t driver_ops = []() {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = Imx227Device::CreateAndBind;
  ops.run_unit_tests = Imx227Device::RunUnitTests;
  return ops;
}();

}  // namespace camera

// clang-format off
ZIRCON_DRIVER_BEGIN(imx227, camera::driver_ops, "imx227", "0.1", 4)
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_COMPOSITE),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, PDEV_VID_SONY),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_PID, PDEV_PID_SONY_IMX227),
    BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_DID, PDEV_DID_CAMERA_SENSOR),
ZIRCON_DRIVER_END(imx227)
