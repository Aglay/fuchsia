// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once
#include <lib/zx/bti.h>
#include <zircon/compiler.h>
#include <ddk/protocol/platform/device.h>
#include <ddk/protocol/platform-device-lib.h>
#include <zircon/assert.h>
#include <ddktl/device.h>
#include <lib/mmio/mmio.h>
#include <hwreg/mmio.h>
#include <fbl/unique_ptr.h>
#include <optional>
#include "common.h"
#include "registers-sysconfig.h"
#include "registers-mutex.h"

namespace mt8167s_display {

class MtSysConfig {
public:
    MtSysConfig() {}

    // Init
    zx_status_t Init(zx_device_t* parent);
    zx_status_t PowerOn(SysConfigModule module);
    zx_status_t PowerDown(SysConfigModule module);
    // This functin will create a default path for the display subsystem
    // The path in shown below. Bracketed statement are either MUX outputs (Multi or Single)
    // or inputs
    // OVL0->[OVL0_MOUT]->[COLOR0_SEL]->COLOR0->CCORR->AAL->GAMMA->DITHER->[DITHER_MOUT]->RDMA0->
    // ->[RDMA0_SOUT]->DSI0_SEL->DSI0
    // TODO(payamm): Add function that can create any valid path
    zx_status_t CreateDefaultPath();
    // Set 0 to the MOUT path of default path
    zx_status_t ClearDefaultPath();
    zx_status_t MutexClear();
    zx_status_t MutexReset();
    zx_status_t MutexEnable();
    zx_status_t MutexDisable();
    zx_status_t MutexSetDefault();
    void PrintRegisters();

private:
    fbl::unique_ptr<ddk::MmioBuffer> syscfg_mmio_;
    fbl::unique_ptr<ddk::MmioBuffer> mutex_mmio_;
    pdev_protocol_t pdev_ = {nullptr, nullptr};
    bool initialized_ = false;
};

} // namespace mt8167s_display
