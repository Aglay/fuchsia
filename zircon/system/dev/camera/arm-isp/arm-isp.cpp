// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "arm-isp.h"
#include "arm-isp-regs.h"
#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/metadata.h>
#include <fbl/alloc_checker.h>
#include <fbl/auto_call.h>
#include <fbl/unique_ptr.h>
#include <memory>
#include <stdlib.h>
#include <threads.h>
#include <unistd.h>
#include <zircon/types.h>

namespace camera {

namespace {

constexpr uint32_t kHiu = 0;
constexpr uint32_t kPowerDomain = 1;
constexpr uint32_t kMemoryDomain = 2;
constexpr uint32_t kReset = 3;
constexpr uint32_t kIsp = 4;

// CLK Shifts & Masks
constexpr uint32_t kClkMuxMask = 0xfff;
constexpr uint32_t kClockEnableShift = 8;

constexpr uint8_t kPing = 0;
constexpr uint8_t kPong = 1;

constexpr uint8_t kCopyToIsp = 0;
constexpr uint8_t kCopyFromIsp = 1;

// ISP memory offsets
constexpr uint32_t kDecompander0PingOffset = 0xAB6C;
constexpr uint32_t kPingConfigSize = 0x17FC0;
constexpr uint32_t kAexpHistStatsOffset = 0x24A8;
constexpr uint32_t kHistSize = 0x2000;
constexpr uint32_t kPingMeteringStatsOffset = 0x44B0;
constexpr uint32_t kMeteringSize = 0x8000;
constexpr uint32_t kLocalBufferSize = (0x18e88 + 0x4000);
constexpr uint32_t kConfigSize = 0x1231C;
} // namespace

void ArmIspDevice::IspHWReset(bool reset) {
    if (reset) {
        reset_mmio_.ClearBits32(1 << 1, RESET4_LEVEL);
    } else {
        reset_mmio_.SetBits32(1 << 1, RESET4_LEVEL);
    }
    // Reference code has a sleep in this path.
    // TODO(braval@) Double check to look into if
    // this sleep is really necessary.
    zx_nanosleep(zx_deadline_after(ZX_MSEC(5)));
}

void ArmIspDevice::PowerUpIsp() {
    // set bit[18-19]=0
    // TODO(braval@) Double check to look into if
    // this sleep is really necessary.
    power_mmio_.ClearBits32(1 << 18 | 1 << 19, AO_RTI_GEN_PWR_SLEEP0);
    zx_nanosleep(zx_deadline_after(ZX_MSEC(5)));

    // set bit[18-19]=0
    power_mmio_.ClearBits32(1 << 18 | 1 << 19, AO_RTI_GEN_PWR_ISO0);

    // MEM_PD_REG0 set 0
    memory_pd_mmio_.Write32(0, HHI_ISP_MEM_PD_REG0);
    // MEM_PD_REG1 set 0
    memory_pd_mmio_.Write32(0, HHI_ISP_MEM_PD_REG1);

    // Refer to reference source code
    hiu_mmio_.Write32(0x5b446585, HHI_CSI_PHY_CNTL0);
    hiu_mmio_.Write32(0x803f4321, HHI_CSI_PHY_CNTL1);

    // Setup Clocks.
    // clear existing values
    hiu_mmio_.ClearBits32(kClkMuxMask, HHI_MIPI_ISP_CLK_CNTL);
    // set the divisor = 1 (writing (1-1) to div field)
    // source for the unused mux = S905D2_FCLK_DIV3   = 3 // 666.7 MHz
    hiu_mmio_.SetBits32(((1 << kClockEnableShift) | 4 << 9),
                        HHI_MIPI_ISP_CLK_CNTL);
}

// Interrupt handler for the ISP.
int ArmIspDevice::IspIrqHandler() {
    zxlogf(INFO, "%s start\n", __func__);
    zx_status_t status = ZX_OK;

    while (running_.load()) {
        status = isp_irq_.wait(NULL);
        if (status != ZX_OK) {
            return status;
        }

        // Handle the Interrupt here.
    }
    return status;
}

void ArmIspDevice::CopyContextInfo(uint8_t config_space,
                                   uint8_t direction) {
    zx_off_t local_offset, device_offset;

    if (config_space == kPing) {
        // PING Context
        local_offset = kDecompander0PingOffset;
        device_offset = kDecompander0PingOffset;
    } else {
        // PONG Context
        local_offset = kDecompander0PingOffset;
        device_offset = kDecompander0PingOffset + kPingConfigSize;
    }

    if (direction == kCopyToIsp) {
        // Copy to ISP from Local Config Buffer
        isp_mmio_.CopyFrom32(isp_mmio_local_, local_offset, device_offset, kConfigSize / 4);
    } else {
        // Copy from ISP to Local Config Buffer
        isp_mmio_local_.CopyFrom32(isp_mmio_, local_offset, device_offset, kConfigSize / 4);
    }
}

void ArmIspDevice::CopyMeteringInfo(uint8_t config_space,
                                    uint8_t direction) {
    zx_off_t local_offset_1, device_offset_1;
    zx_off_t local_offset_2, device_offset_2;

    if (config_space == kPing) {
        // PING Context
        local_offset_1 = kAexpHistStatsOffset;
        device_offset_1 = kAexpHistStatsOffset;

        local_offset_2 = kPingMeteringStatsOffset;
        device_offset_2 = kPingMeteringStatsOffset;
    } else {
        // PONG Context
        local_offset_1 = kAexpHistStatsOffset;
        device_offset_1 = kAexpHistStatsOffset;

        local_offset_2 = kPingMeteringStatsOffset;
        device_offset_2 = kPingMeteringStatsOffset + kPingConfigSize;
    }

    if (direction == kCopyToIsp) {
        // Copy to ISP from Local Config Buffer
        isp_mmio_.CopyFrom32(isp_mmio_local_, local_offset_1, device_offset_1, kHistSize / 4);
        isp_mmio_.CopyFrom32(isp_mmio_local_, local_offset_2, device_offset_2, kMeteringSize / 4);
    } else {
        // Copy from ISP to Local Config Buffer
        isp_mmio_local_.CopyFrom32(isp_mmio_, local_offset_1, device_offset_1, kHistSize / 4);
        isp_mmio_local_.CopyFrom32(isp_mmio_, local_offset_2, device_offset_2, kMeteringSize / 4);
    }
}

zx_status_t ArmIspDevice::IspContextInit() {
    // This is actually writing to the HW
    IspLoadSeq_settings();

    // This is being written to the local_config_buffer_
    IspLoadSeq_settings_context();

    statsMgr_ = camera::StatsManager::Create(isp_mmio_.View(0),
                                             isp_mmio_local_,
                                             sensor_callbacks_);
    if (statsMgr_ == nullptr) {
        zxlogf(ERROR, "%s: Unable to start StatsManager \n", __func__);
        return ZX_ERR_NO_MEMORY;
    }

    // Call custom_init()
    IspLoadCustomSequence();

    // Input port safe start
    InputPort_Config3::Get()
        .ReadFrom(&isp_mmio_)
        .set_mode_request(1)
        .WriteTo(&isp_mmio_);

    return ZX_OK;
}

zx_status_t ArmIspDevice::InitIsp() {
    // The ISP and MIPI module is in same power domain.
    // So if we don't call the power sequence of ISP, the mipi module
    // won't work and it will block accesses to the  mipi register block.
    PowerUpIsp();

    IspHWReset(true);

    // Start ISP Interrupt Handling Thread.
    auto start_thread = [](void* arg) -> int {
        return static_cast<ArmIspDevice*>(arg)->IspIrqHandler();
    };

    running_.store(true);
    int rc = thrd_create_with_name(&irq_thread_,
                                   start_thread,
                                   this,
                                   "isp_irq_thread");
    if (rc != thrd_success) {
        return ZX_ERR_INTERNAL;
    }

    IspHWReset(false);

    // validate the ISP product ID
    if (Id_Product::Get().ReadFrom(&isp_mmio_).value() != PRODUCT_ID_DEFAULT) {
        zxlogf(ERROR, "%s: Unknown product ID\n", __func__);
        return ZX_ERR_NOT_SUPPORTED;
    }

    // Mask all IRQs
    IspGlobalInterrupt_MaskVector::Get().ReadFrom(&isp_mmio_).mask_all().WriteTo(&isp_mmio_);

    // Now copy all ping config settings & metering settings and store it.
    CopyContextInfo(kPing, kCopyFromIsp);

    zx_status_t status = IspContextInit();
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: IspContextInit failed %d\n", __func__, status);
    }

    // Copy current context to ISP
    CopyContextInfo(kPing, kCopyToIsp);
    CopyContextInfo(kPong, kCopyToIsp);

    while (IspGlobalInterrupt_StatusVector::Get().ReadFrom(&isp_mmio_).reg_value()) {
        // driver is initialized. we can start processing interrupts
        // wait until irq mask is cleared and start processing
        IspGlobalInterrupt_Clear::Get().ReadFrom(&isp_mmio_).set_value(0).WriteTo(&isp_mmio_);
        IspGlobalInterrupt_Clear::Get().ReadFrom(&isp_mmio_).set_value(1).WriteTo(&isp_mmio_);
    }

    IspGlobalInterrupt_MaskVector::Get()
        .ReadFrom(&isp_mmio_)
        .set_isp_start(0)
        .set_ctx_management_error(0)
        .set_broken_frame_error(0)
        .set_wdg_timer_timed_out(0)
        .set_frame_collision_error(0)
        .set_dma_error_interrupt(0)
        .WriteTo(&isp_mmio_);

    // put ping pong in slave mode
    // SW only mode
    IspGlobal_Config3::Get()
        .ReadFrom(&isp_mmio_)
        .set_mcu_override_config_select(1)
        .WriteTo(&isp_mmio_);

    return ZX_OK;
}

// static
zx_status_t ArmIspDevice::Create(zx_device_t* parent, isp_callbacks_t sensor_callbacks) {

    ddk::PDev pdev(parent);
    if (!pdev.is_valid()) {
        zxlogf(ERROR, "%s: ZX_PROTOCOL_PDEV not available\n", __FILE__);
        return ZX_ERR_NO_RESOURCES;
    }
    std::optional<ddk::MmioBuffer> hiu_mmio;
    zx_status_t status = pdev.MapMmio(kHiu, &hiu_mmio);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: pdev_.MapMmio failed %d\n", __func__, status);
        return status;
    }

    std::optional<ddk::MmioBuffer> power_mmio;
    status = pdev.MapMmio(kPowerDomain, &power_mmio);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: pdev_.MapMmio failed %d\n", __func__, status);
        return status;
    }

    std::optional<ddk::MmioBuffer> memory_pd_mmio;
    status = pdev.MapMmio(kMemoryDomain, &memory_pd_mmio);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: pdev_.MapMmio failed %d\n", __func__, status);
        return status;
    }

    std::optional<ddk::MmioBuffer> reset_mmio;
    status = pdev.MapMmio(kReset, &reset_mmio);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: pdev_.MapMmio failed %d\n", __func__, status);
        return status;
    }

    std::optional<ddk::MmioBuffer> isp_mmio;
    status = pdev.MapMmio(kIsp, &isp_mmio);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: pdev_.MapMmio failed %d\n", __func__, status);
        return status;
    }

    zx::interrupt isp_irq;
    status = pdev.GetInterrupt(0, &isp_irq);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: pdev_.GetInterrupt failed %d\n", __func__, status);
        return status;
    }

    // Allocate buffers for ISP SW configuration and metering information.
    fbl::AllocChecker ac;
    mmio_buffer_t local_mmio_buffer;
    local_mmio_buffer.vaddr = new (static_cast<std::align_val_t>(alignof(uint32_t)),
                                   &ac) char[kLocalBufferSize];
    local_mmio_buffer.size = kLocalBufferSize;
    local_mmio_buffer.vmo = ZX_HANDLE_INVALID;
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }

    auto isp_device = std::unique_ptr<ArmIspDevice>(new (&ac) ArmIspDevice(
        parent,
        std::move(*hiu_mmio),
        std::move(*power_mmio),
        std::move(*memory_pd_mmio),
        std::move(*reset_mmio),
        std::move(*isp_mmio),
        local_mmio_buffer,
        std::move(isp_irq),
        sensor_callbacks));
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }

    isp_device->InitIsp();

    status = isp_device->DdkAdd("arm-isp");
    if (status != ZX_OK) {
        zxlogf(ERROR, "arm-isp: Could not create arm-isp device: %d\n", status);
        return status;
    } else {
        zxlogf(INFO, "arm-isp: Added arm-isp device\n");
    }

    // isp_device intentionally leaked as it is now held by DevMgr.
    __UNUSED auto ptr = isp_device.release();

    return status;
}

ArmIspDevice::~ArmIspDevice() {
    free(isp_mmio_local_.get());
    running_.store(false);
    thrd_join(irq_thread_, NULL);
    isp_irq_.destroy();
}

void ArmIspDevice::DdkUnbind() {
    ShutDown();
    DdkRemove();
}

void ArmIspDevice::DdkRelease() {
    delete this;
}

void ArmIspDevice::ShutDown() {
}

} // namespace camera
