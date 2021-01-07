// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_GRAPHICS_DISPLAY_DRIVERS_MT8167S_DISPLAY_MT_DSI_HOST_H_
#define SRC_GRAPHICS_DISPLAY_DRIVERS_MT8167S_DISPLAY_MT_DSI_HOST_H_

#include <fuchsia/hardware/dsiimpl/cpp/banjo.h>
#include <fuchsia/hardware/gpio/cpp/banjo.h>
#include <fuchsia/hardware/power/cpp/banjo.h>
#include <lib/device-protocol/pdev.h>
#include <lib/mmio/mmio.h>
#include <lib/zx/bti.h>
#include <zircon/assert.h>
#include <zircon/compiler.h>

#include <memory>
#include <optional>

#include <ddktl/device.h>
#include <hwreg/mmio.h>

#include "common.h"
#include "lcd.h"
#include "mt-sysconfig.h"
#include "registers-mipiphy.h"

namespace mt8167s_display {

// [Ovl] --> [Clr] --> [Clr Correction] --> [AAL] --> [Gamma] --> [Dither] --> [RDMA] --> [DSI]

// The DSI engine is responsible for fetching data from the display pipe and outputting it to
// the MIPI PHY. The DSI IP is mediatek specific. However, it does follow the MIPI DSI SPEC. This
// class is responsible for setting up the MIPI-PHY and use the dsi-mt driver to perform
// DSI specific operations.

class MtDsiHost {
 public:
  MtDsiHost(ddk::PDev pdev, uint32_t height, uint32_t width, uint32_t panel_type)
      : pdev_(pdev), height_(height), width_(width), panel_type_(panel_type) {
    ZX_ASSERT(height_ < kMaxHeight);
    ZX_ASSERT(width_ < kMaxWidth);
  }

  zx_status_t Init(const ddk::DsiImplProtocolClient* dsi, const ddk::GpioProtocolClient* gpio,
                   const ddk::PowerProtocolClient* power);

  // Used for Unit Testing
  zx_status_t Init(std::optional<ddk::MmioBuffer> mmio, std::unique_ptr<Lcd> lcd,
                   const ddk::DsiImplProtocolClient* dsi, const ddk::GpioProtocolClient* gpio,
                   const ddk::PowerProtocolClient* power) {
    mipi_tx_mmio_ = std::move(mmio);
    lcd_ = std::move(lcd);
    dsiimpl_ = *dsi;
    power_ = *power;
    initialized_ = true;
    return ZX_OK;
  }

  zx_status_t Config(const display_setting_t& disp_setting);
  zx_status_t Start();
  zx_status_t Shutdown(std::unique_ptr<MtSysConfig>& syscfg);
  zx_status_t PowerOn(std::unique_ptr<MtSysConfig>& syscfg);

  bool IsHostOn() {
    ZX_DEBUG_ASSERT(initialized_);
    // PLL EN is the safest bit to read to see if the host is on or not. If Host is trully
    // off, we cannot read any of the DSI IP registers. Furthermore, the DSI clock enable bit
    // within the syscfg register always returns 0 regardless of whether it's really on or not
    return (MipiTxPllCon0Reg::Get().ReadFrom(&(*mipi_tx_mmio_)).pll_en() == 1);
  }

  void PrintRegisters();

 private:
  void ConfigMipiPll(uint32_t pll_clock, uint32_t lane_num);
  void PowerOffMipiTx();

  ddk::PDev pdev_;
  uint32_t height_;  // display height
  uint32_t width_;   // display width
  uint32_t panel_type_;
  std::optional<ddk::MmioBuffer> mipi_tx_mmio_;
  zx::bti bti_;
  ddk::DsiImplProtocolClient dsiimpl_;
  ddk::PowerProtocolClient power_;
  std::unique_ptr<Lcd> lcd_;

  bool initialized_ = false;
};

}  // namespace mt8167s_display

#endif  // SRC_GRAPHICS_DISPLAY_DRIVERS_MT8167S_DISPLAY_MT_DSI_HOST_H_
