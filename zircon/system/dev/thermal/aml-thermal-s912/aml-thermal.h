// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/device.h>
#include <ddktl/device.h>
#include <ddktl/protocol/empty-protocol.h>
#include <ddktl/protocol/gpio.h>
#include <ddktl/protocol/platform/device.h>
#include <ddktl/protocol/scpi.h>
#include <fuchsia/hardware/thermal/c/fidl.h>
#include <lib/fidl-utils/bind.h>
#include <lib/sync/completion.h>
#include <lib/zx/port.h>
#include <threads.h>
#include <zircon/device/thermal.h>

#include <utility>

#pragma once

namespace thermal {

enum FanLevel {
    FAN_L0,
    FAN_L1,
    FAN_L2,
    FAN_L3,
};

class AmlThermal;
using DeviceType = ddk::Device<AmlThermal, ddk::Ioctlable, ddk::Messageable, ddk::Unbindable>;

// AmlThermal implements the s912 AmLogic thermal driver.
class AmlThermal : public DeviceType, public ddk::EmptyProtocol<ZX_PROTOCOL_THERMAL> {
public:
    AmlThermal(zx_device_t* device,
               const ddk::PDevProtocolClient& pdev,
               const gpio_protocol_t& fan0_gpio_proto,
               const gpio_protocol_t& fan1_gpio_proto,
               const scpi_protocol_t& scpi_proto,
               const uint32_t& sensor_id,
               zx::port& port)
        : DeviceType(device),
          pdev_(pdev),
          fan0_gpio_(&fan0_gpio_proto),
          fan1_gpio_(&fan1_gpio_proto),
          scpi_(&scpi_proto),
          sensor_id_(sensor_id),
          port_(std::move(port)) {}

    // Create and bind a driver instance.
    static zx_status_t Create(zx_device_t* device);

    // Perform post-construction runtime initialization.
    zx_status_t Init();

    // Ddk-required methods.
    zx_status_t DdkIoctl(uint32_t op, const void* in_buf, size_t in_len,
                         void* out_buf, size_t out_len, size_t* actual);
    zx_status_t DdkMessage(fidl_msg_t* msg, fidl_txn_t* txn);
    void DdkUnbind();
    void DdkRelease();

private:
    zx_status_t GetInfo(fidl_txn_t* txn);
    zx_status_t GetDeviceInfo(fidl_txn_t* txn);
    zx_status_t GetDvfsInfo(fuchsia_hardware_thermal_PowerDomain power_domain, fidl_txn_t* txn);
    zx_status_t GetTemperature(fidl_txn_t* txn);
    zx_status_t GetStateChangeEvent(fidl_txn_t* txn);
    zx_status_t GetStateChangePort(fidl_txn_t* txn);
    zx_status_t SetTrip(uint16_t op_idx, fuchsia_hardware_thermal_PowerDomain power_domain,
                        fidl_txn_t* txn);
    zx_status_t GetDvfsOperatingPoint(fuchsia_hardware_thermal_PowerDomain power_domain,
                                      fidl_txn_t* txn);
    zx_status_t SetDvfsOperatingPoint(uint16_t op_idx,
                                      fuchsia_hardware_thermal_PowerDomain power_domain,
                                      fidl_txn_t* txn);
    zx_status_t GetFanLevel(fidl_txn_t* txn);
    zx_status_t SetFanLevel(uint32_t fan_level, fidl_txn_t* txn);

    static constexpr fuchsia_hardware_thermal_Device_ops_t fidl_ops = {
        .GetInfo = fidl::Binder<AmlThermal>::BindMember<&AmlThermal::GetInfo>,
        .GetDeviceInfo = fidl::Binder<AmlThermal>::BindMember<&AmlThermal::GetDeviceInfo>,
        .GetDvfsInfo = fidl::Binder<AmlThermal>::BindMember<&AmlThermal::GetDvfsInfo>,
        .GetTemperature = fidl::Binder<AmlThermal>::BindMember<&AmlThermal::GetTemperature>,
        .GetStateChangeEvent =
            fidl::Binder<AmlThermal>::BindMember<&AmlThermal::GetStateChangeEvent>,
        .GetStateChangePort = fidl::Binder<AmlThermal>::BindMember<&AmlThermal::GetStateChangePort>,
        .SetTrip = fidl::Binder<AmlThermal>::BindMember<&AmlThermal::SetTrip>,
        .GetDvfsOperatingPoint =
            fidl::Binder<AmlThermal>::BindMember<&AmlThermal::GetDvfsOperatingPoint>,
        .SetDvfsOperatingPoint =
            fidl::Binder<AmlThermal>::BindMember<&AmlThermal::SetDvfsOperatingPoint>,
        .GetFanLevel = fidl::Binder<AmlThermal>::BindMember<&AmlThermal::GetFanLevel>,
        .SetFanLevel = fidl::Binder<AmlThermal>::BindMember<&AmlThermal::SetFanLevel>,
    };

    // Notification thread implementation.
    int Worker();

    // Set the fans to the given level.
    zx_status_t SetFanLevel(FanLevel level);

    // Notify the thermal daemon of the current settings.
    zx_status_t NotifyThermalDaemon(uint32_t trip_point) const;

    ddk::PDevProtocolClient pdev_;
    ddk::GpioProtocolClient fan0_gpio_;
    ddk::GpioProtocolClient fan1_gpio_;
    ddk::ScpiProtocolClient scpi_;

    uint32_t sensor_id_;
    zx::port port_;

    thrd_t worker_ = {};
    fuchsia_hardware_thermal_ThermalDeviceInfo info_ = {};
    FanLevel fan_level_ = FAN_L0;
    uint32_t temperature_ = 0;
    sync_completion quit_;
    uint32_t cur_bigcluster_opp_idx_ = 0;
    uint32_t cur_littlecluster_opp_idx_ = 0;
};

} // namespace thermal
