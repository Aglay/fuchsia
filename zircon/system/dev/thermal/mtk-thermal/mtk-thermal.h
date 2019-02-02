// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <threads.h>

#include <ddk/protocol/platform-device-lib.h>
#include <ddktl/device.h>
#include <lib/mmio/mmio.h>
#include <ddktl/protocol/clk.h>
#include <ddktl/protocol/empty-protocol.h>
#include <fbl/mutex.h>
#include <lib/zx/interrupt.h>
#include <lib/zx/port.h>
#include <zircon/device/thermal.h>

namespace thermal {

class MtkThermal;
using DeviceType = ddk::Device<MtkThermal, ddk::Ioctlable>;

class MtkThermal : public DeviceType, public ddk::EmptyProtocol<ZX_PROTOCOL_THERMAL> {
public:
    virtual ~MtkThermal() = default;

    static zx_status_t Create(void* ctx, zx_device_t* parent);

    void DdkRelease();

    zx_status_t DdkIoctl(uint32_t op, const void* in_buf, size_t in_len, void* out_buf,
                         size_t out_len, size_t* actual);

    // Visible for testing.
    zx_status_t Init();
    zx_status_t StartThread();
    virtual void StopThread();

protected:
    // Visible for testing.
    MtkThermal(zx_device_t* parent, ddk::MmioBuffer mmio, ddk::MmioBuffer fuse_mmio,
               ddk::MmioBuffer pll_mmio, ddk::MmioBuffer pmic_mmio,
               const ddk::ClkProtocolClient& clk, uint32_t clk_count,
               const thermal_device_info_t& thermal_info, zx::port port, zx::interrupt irq)
        : DeviceType(parent), mmio_(std::move(mmio)), fuse_mmio_(std::move(fuse_mmio)),
          pll_mmio_(std::move(pll_mmio)), pmic_mmio_(std::move(pmic_mmio)), clk_(clk),
          clk_count_(clk_count), thermal_info_(thermal_info), port_(std::move(port)),
          irq_(std::move(irq)) {}

    virtual void PmicWrite(uint16_t data, uint32_t addr);

    virtual uint32_t GetTemperature();

    virtual zx_status_t SetDvfsOpp(const dvfs_info_t* opp);

    virtual zx_status_t SetTripPoint(size_t trip_pt);

    virtual zx_status_t WaitForInterrupt();

    int JoinThread() { return thrd_join(thread_, nullptr); }

    ddk::MmioBuffer mmio_;
    ddk::MmioBuffer fuse_mmio_;
    ddk::MmioBuffer pll_mmio_;
    ddk::MmioBuffer pmic_mmio_;

private:
    uint32_t RawToTemperature(uint32_t raw, uint32_t sensor);
    uint32_t TemperatureToRaw(uint32_t temp, uint32_t sensor);

    uint32_t GetRawHot(uint32_t temp);
    uint32_t GetRawCold(uint32_t temp);

    int Thread();

    ddk::ClkProtocolClient clk_;
    const uint32_t clk_count_;
    const thermal_device_info_t thermal_info_;
    uint32_t current_opp_idx_ = 0;
    zx::port port_;
    zx::interrupt irq_;
    thrd_t thread_;
    fbl::Mutex dvfs_lock_;
};

}  // namespace thermal
