// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CAMERA_DRIVERS_SENSORS_IMX227_IMX227_H_
#define SRC_CAMERA_DRIVERS_SENSORS_IMX227_IMX227_H_

#include <lib/device-protocol/i2c-channel.h>
#include <lib/device-protocol/pdev.h>
#include <lib/fit/result.h>

#include <array>
#include <mutex>

#include <ddk/platform-defs.h>
#include <ddktl/device.h>
#include <ddktl/protocol/camerasensor.h>
#include <ddktl/protocol/clock.h>
#include <ddktl/protocol/composite.h>
#include <ddktl/protocol/empty-protocol.h>
#include <ddktl/protocol/gpio.h>
#include <ddktl/protocol/mipicsi.h>

namespace camera {

struct SensorCtx {
  // TODO(braval): Add details for each one of these
  //               and also remove unused ones.
  uint32_t again_limit;
  uint32_t int_max;
  uint32_t dgain_limit;
  uint32_t wdr_mode;
  uint32_t gain_cnt;
  uint32_t t_height;
  uint32_t int_time_limit;
  uint32_t t_height_old;
  uint16_t int_time;
  uint16_t VMAX;
  uint16_t HMAX;
  uint16_t dgain_old;
  uint16_t int_time_min;
  uint16_t again_old;
  std::array<uint16_t, 2> dgain;
  std::array<uint16_t, 2> again;
  uint8_t seq_width;
  uint8_t streaming_flag;
  uint8_t again_delay;
  uint8_t again_change;
  uint8_t dgain_delay;
  uint8_t dgain_change;
  uint8_t change_flag;
  uint8_t hdr_flag;
  camera_sensor_info_t param;
};

class Imx227Device;
using DeviceType = ddk::Device<Imx227Device, ddk::UnbindableNew>;

class Imx227Device : public DeviceType,
                     public ddk::CameraSensorProtocol<Imx227Device, ddk::base_protocol> {
 public:
  enum {
    FRAGMENT_PDEV,
    FRAGMENT_MIPICSI,
    FRAGMENT_I2C,
    FRAGMENT_GPIO_VANA,
    FRAGMENT_GPIO_VDIG,
    FRAGMENT_GPIO_CAM_RST,
    FRAGMENT_CLK24,
    FRAGMENT_COUNT,
  };

  Imx227Device(zx_device_t* device, zx_device_t* i2c, zx_device_t* gpio_vana,
               zx_device_t* gpio_vdig, zx_device_t* gpio_cam_rst, zx_device_t* clk24,
               zx_device_t* mipicsi)
      : DeviceType(device),
        i2c_(i2c),
        gpio_vana_enable_(gpio_vana),
        gpio_vdig_enable_(gpio_vdig),
        gpio_cam_rst_(gpio_cam_rst),
        clk24_(clk24),
        mipi_(mipicsi) {}

  static zx_status_t Create(zx_device_t* parent, std::unique_ptr<Imx227Device>* device_out);
  static zx_status_t CreateAndBind(void* ctx, zx_device_t* parent);
  static bool RunUnitTests(void* ctx, zx_device_t* parent, zx_handle_t channel);

  // Methods required by the ddk mixins
  void DdkUnbindNew(ddk::UnbindTxn txn);
  void DdkRelease();

  // Testing interface will need to use this to check the status of the sensor.
  bool IsSensorInitialized() const { return initialized_; }

  // Methods for ZX_PROTOCOL_CAMERA_SENSOR
  zx_status_t CameraSensorInit();
  void CameraSensorDeInit();
  zx_status_t CameraSensorSetMode(uint8_t mode);
  zx_status_t CameraSensorStartStreaming();
  zx_status_t CameraSensorStopStreaming();
  int32_t CameraSensorSetAnalogGain(int32_t gain);
  int32_t CameraSensorSetDigitalGain(int32_t gain);
  zx_status_t CameraSensorSetIntegrationTime(int32_t int_time);
  zx_status_t CameraSensorUpdate();
  zx_status_t CameraSensorGetInfo(camera_sensor_info_t* out_info);
  zx_status_t CameraSensorGetSupportedModes(camera_sensor_mode_t* out_modes_list,
                                            size_t modes_count, size_t* out_modes_actual);
  bool CameraSensorIsPoweredUp();

  // OTP

  //  Read the sensor's entire OTP memory.
  //
  //  Returns:
  //    A result with a vmo containing the OTP blob if the read succeeded. Otherwise returns a
  //    result with an error code.
  fit::result<zx::vmo, zx_status_t> OtpRead();

  //  Validates the integrity of the data written to the OTP. A checksum is calculated from the
  //  written data and checked against a hard-coded value.
  //
  //  Args:
  //    |vmo|   VMO pointer of data to be validated
  //
  //  Returns:
  //    Whether the OTP data was validated successfully.
  static bool OtpValidate(const zx::vmo* vmo);

 private:
  // I2C Helpers
  // Returns ZX_OK and an uint16_t value if the read succeeds.
  // Returns error if the I2C read fails.
  fit::result<uint16_t, zx_status_t> Read16(uint16_t addr) __TA_REQUIRES(lock_);
  // Returns ZX_OK and an uint8_t value if the read succeeds.
  // Returns error if the I2C read fails.
  fit::result<uint8_t, zx_status_t> Read8(uint16_t addr) __TA_REQUIRES(lock_);
  // Returns ZX_OK if the write is successful otherwise returns an error.
  zx_status_t Write8(uint16_t addr, uint8_t val) __TA_REQUIRES(lock_);

  // Other
  zx_status_t InitPdev();
  zx_status_t InitSensor(uint8_t idx) __TA_REQUIRES(lock_);
  void HwInit() __TA_REQUIRES(lock_);
  void HwDeInit() __TA_REQUIRES(lock_);
  void ShutDown();
  bool ValidateSensorID() __TA_REQUIRES(lock_);
  bool IsSensorOutOfReset() __TA_REQUIRES(lock_) { return ValidateSensorID(); }

  // Sensor Context
  SensorCtx ctx_ __TA_GUARDED(lock_);

  // Protocols
  ddk::I2cChannel i2c_;
  ddk::GpioProtocolClient gpio_vana_enable_;
  ddk::GpioProtocolClient gpio_vdig_enable_;
  ddk::GpioProtocolClient gpio_cam_rst_;
  ddk::ClockProtocolClient clk24_;
  ddk::MipiCsiProtocolClient mipi_;

  // Sensor Status
  bool initialized_ = false;

  std::mutex lock_;
};

}  // namespace camera

#endif  // SRC_CAMERA_DRIVERS_SENSORS_IMX227_IMX227_H_
