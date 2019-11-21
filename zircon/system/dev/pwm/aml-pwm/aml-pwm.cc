// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aml-pwm.h"

#include <lib/device-protocol/pdev.h>

#include <ddk/binding.h>
#include <ddk/protocol/platform/bus.h>
#include <soc/aml-a113/a113-pwm.h>
#include <soc/aml-s905d2/s905d2-pwm.h>
#include <soc/aml-t931/t931-pwm.h>

namespace pwm {

namespace {

// Input clock frequency
constexpr uint32_t kXtalFreq = 24'000'000;
// Nanoseconds per second
constexpr uint32_t kNsecPerSec = 1'000'000'000;

constexpr uint64_t DivideRounded(uint64_t num, uint64_t denom) {
  return (num + (denom / 2)) / denom;
}

void DutyCycleToClockCount(float duty_cycle, uint32_t period_ns, uint16_t* high_count,
                           uint16_t* low_count) {
  constexpr uint64_t kNanosecondsPerClock = kNsecPerSec / kXtalFreq;

  // Calculate the high and low count first based on the duty cycle requested.
  const auto high_time_ns =
      static_cast<uint64_t>((duty_cycle * static_cast<float>(period_ns)) / 100.0);
  const auto period_count = static_cast<uint16_t>(period_ns / kNanosecondsPerClock);

  const auto duty_count = static_cast<uint16_t>(DivideRounded(high_time_ns, kNanosecondsPerClock));

  *high_count = duty_count;
  *low_count = static_cast<uint16_t>(period_count - duty_count);
  if (duty_count != period_count && duty_count != 0) {
    (*high_count)--;
    (*low_count)--;
  }
}

}  // namespace

zx_status_t AmlPwm::PwmImplSetConfig(uint32_t idx, const pwm_config_t* config) {
  if (idx > 1) {
    return ZX_ERR_INVALID_ARGS;
  }
  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t AmlPwm::PwmImplEnable(uint32_t idx) {
  if (idx > 1) {
    return ZX_ERR_INVALID_ARGS;
  }
  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t AmlPwm::PwmImplDisable(uint32_t idx) {
  if (idx > 1) {
    return ZX_ERR_INVALID_ARGS;
  }
  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t AmlPwm::SetMode(uint32_t idx, Mode mode) {
  if (mode >= UNKNOWN) {
    return ZX_ERR_INVALID_ARGS;
  }

  fbl::AutoLock lock(&locks_[REG_MISC]);
  auto misc_reg = MiscReg::Get().ReadFrom(&mmio_);
  if (idx % 2) {
    misc_reg.set_en_b(mode == ON || mode == TWO_TIMER)
        .set_ds_en_b(mode == DELTA_SIGMA)
        .set_en_b2(mode == TWO_TIMER);
  } else {
    misc_reg.set_en_a(mode == ON || mode == TWO_TIMER)
        .set_ds_en_a(mode == DELTA_SIGMA)
        .set_en_a2(mode == TWO_TIMER);
  }
  misc_reg.WriteTo(&mmio_);

  return ZX_OK;
}

zx_status_t AmlPwm::SetDutyCycle(uint32_t idx, uint32_t period_ns, float duty_cycle) {
  if (duty_cycle > 100 || duty_cycle < 0) {
    return ZX_ERR_INVALID_ARGS;
  }

  // Write duty cycle to registers
  uint16_t high_count = 0, low_count = 0;
  DutyCycleToClockCount(duty_cycle, period_ns, &high_count, &low_count);
  if (idx % 2) {
    fbl::AutoLock lock(&locks_[REG_B]);
    DutyCycleReg::GetB().ReadFrom(&mmio_).set_high(high_count).set_low(low_count).WriteTo(&mmio_);
  } else {
    fbl::AutoLock lock(&locks_[REG_A]);
    DutyCycleReg::GetA().ReadFrom(&mmio_).set_high(high_count).set_low(low_count).WriteTo(&mmio_);
  }

  return ZX_OK;
}

zx_status_t AmlPwm::SetDutyCycle2(uint32_t idx, uint32_t period_ns, float duty_cycle) {
  if (duty_cycle > 100 || duty_cycle < 0) {
    return ZX_ERR_INVALID_ARGS;
  }

  // Write duty cycle to registers
  uint16_t high_count = 0, low_count = 0;
  DutyCycleToClockCount(duty_cycle, period_ns, &high_count, &low_count);
  if (idx % 2) {
    fbl::AutoLock lock(&locks_[REG_B2]);
    DutyCycleReg::GetB2().ReadFrom(&mmio_).set_high(high_count).set_low(low_count).WriteTo(&mmio_);
  } else {
    fbl::AutoLock lock(&locks_[REG_A2]);
    DutyCycleReg::GetA2().ReadFrom(&mmio_).set_high(high_count).set_low(low_count).WriteTo(&mmio_);
  }

  return ZX_OK;
}

zx_status_t AmlPwm::Invert(uint32_t idx, bool on) {
  fbl::AutoLock lock(&locks_[REG_MISC]);
  auto misc_reg = MiscReg::Get().ReadFrom(&mmio_);
  if (idx % 2) {
    misc_reg.set_inv_en_b(on);
  } else {
    misc_reg.set_inv_en_a(on);
  }
  misc_reg.WriteTo(&mmio_);

  return ZX_OK;
}

zx_status_t AmlPwm::EnableHiZ(uint32_t idx, bool on) {
  fbl::AutoLock lock(&locks_[REG_MISC]);
  auto misc_reg = MiscReg::Get().ReadFrom(&mmio_);
  if (idx % 2) {
    misc_reg.set_hiz_b(on);
  } else {
    misc_reg.set_hiz_a(on);
  }
  misc_reg.WriteTo(&mmio_);

  return ZX_OK;
}

zx_status_t AmlPwm::EnableClock(uint32_t idx, bool on) {
  fbl::AutoLock lock(&locks_[REG_MISC]);
  auto misc_reg = MiscReg::Get().ReadFrom(&mmio_);
  if (idx % 2) {
    misc_reg.set_clk_en_b(on);
  } else {
    misc_reg.set_clk_en_a(on);
  }
  misc_reg.WriteTo(&mmio_);

  return ZX_OK;
}

zx_status_t AmlPwm::EnableConst(uint32_t idx, bool on) {
  fbl::AutoLock lock(&locks_[REG_MISC]);
  auto misc_reg = MiscReg::Get().ReadFrom(&mmio_);
  if (idx % 2) {
    misc_reg.set_constant_en_b(on);
  } else {
    misc_reg.set_constant_en_a(on);
  }
  misc_reg.WriteTo(&mmio_);

  return ZX_OK;
}

zx_status_t AmlPwm::SetClock(uint32_t idx, uint8_t sel) {
  fbl::AutoLock lock(&locks_[REG_MISC]);
  auto misc_reg = MiscReg::Get().ReadFrom(&mmio_);
  if (idx % 2) {
    misc_reg.set_clk_sel_b(sel);
  } else {
    misc_reg.set_clk_sel_a(sel);
  }
  misc_reg.WriteTo(&mmio_);

  return ZX_OK;
}

zx_status_t AmlPwm::SetClockDivider(uint32_t idx, uint8_t div) {
  fbl::AutoLock lock(&locks_[REG_MISC]);
  auto misc_reg = MiscReg::Get().ReadFrom(&mmio_);
  if (idx % 2) {
    misc_reg.set_clk_div_b(div);
  } else {
    misc_reg.set_clk_div_a(div);
  }
  misc_reg.WriteTo(&mmio_);

  return ZX_OK;
}

zx_status_t AmlPwm::EnableBlink(uint32_t idx, bool on) {
  fbl::AutoLock lock(&locks_[REG_BLINK]);
  auto blink_reg = BlinkReg::Get().ReadFrom(&mmio_);
  if (idx % 2) {
    blink_reg.set_enable_b(on);
  } else {
    blink_reg.set_enable_a(on);
  }
  blink_reg.WriteTo(&mmio_);

  return ZX_OK;
}

zx_status_t AmlPwm::SetBlinkTimes(uint32_t idx, uint8_t times) {
  fbl::AutoLock lock(&locks_[REG_BLINK]);
  auto blink_reg = BlinkReg::Get().ReadFrom(&mmio_);
  if (idx % 2) {
    blink_reg.set_times_b(times);
  } else {
    blink_reg.set_times_a(times);
  }
  blink_reg.WriteTo(&mmio_);

  return ZX_OK;
}

zx_status_t AmlPwm::SetDSSetting(uint32_t idx, uint16_t val) {
  fbl::AutoLock lock(&locks_[REG_DS]);
  auto ds_reg = DeltaSigmaReg::Get().ReadFrom(&mmio_);
  if (idx % 2) {
    ds_reg.set_b(val);
  } else {
    ds_reg.set_a(val);
  }
  ds_reg.WriteTo(&mmio_);

  return ZX_OK;
}

zx_status_t AmlPwm::SetTimers(uint32_t idx, uint8_t timer1, uint8_t timer2) {
  fbl::AutoLock lock(&locks_[REG_TIME]);
  auto time_reg = TimeReg::Get().ReadFrom(&mmio_);
  if (idx % 2) {
    time_reg.set_b1(timer1).set_b2(timer2);
  } else {
    time_reg.set_a1(timer1).set_a2(timer2);
  }
  time_reg.WriteTo(&mmio_);

  return ZX_OK;
}

zx_status_t AmlPwmDevice::Create(void* ctx, zx_device_t* parent) {
  fbl::AllocChecker ac;
  std::unique_ptr<AmlPwmDevice> device(new (&ac) AmlPwmDevice(parent));
  if (!ac.check()) {
    zxlogf(ERROR, "%s: device object alloc failed\n", __func__);
    return ZX_ERR_NO_MEMORY;
  }

  zx_status_t status = ZX_OK;
  if ((status = device->Init(parent)) != ZX_OK) {
    zxlogf(ERROR, "%s: Init failed\n", __func__);
    return status;
  }

  if ((status = device->DdkAdd("aml-pwm-device", 0, nullptr, 0, ZX_PROTOCOL_PWM_IMPL, nullptr,
                               ZX_HANDLE_INVALID, nullptr, 0)) != ZX_OK) {
    zxlogf(ERROR, "%s: DdkAdd failed\n", __func__);
    return status;
  }

  __UNUSED auto* unused = device.release();

  return ZX_OK;
}

zx_status_t AmlPwmDevice::Init(zx_device_t* parent) {
  zx_status_t status = ZX_OK;

  ddk::PDev pdev(parent);
  for (uint32_t i = 0;; i++) {
    std::optional<ddk::MmioBuffer> mmio;
    if ((status = pdev.MapMmio(i, &mmio)) != ZX_OK) {
      break;
    }
    pwms_.push_back(std::make_unique<AmlPwm>(*std::move(mmio)));
  }

  return ZX_OK;
}

zx_status_t AmlPwmDevice::PwmImplSetConfig(uint32_t idx, const pwm_config_t* config) {
  if (idx >= pwms_.size() * 2 || config == nullptr || config->mode_config_buffer == nullptr) {
    return ZX_ERR_INVALID_ARGS;
  }
  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t AmlPwmDevice::PwmImplEnable(uint32_t idx) {
  if (idx >= pwms_.size() * 2) {
    return ZX_ERR_INVALID_ARGS;
  }
  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t AmlPwmDevice::PwmImplDisable(uint32_t idx) {
  if (idx >= pwms_.size() * 2) {
    return ZX_ERR_INVALID_ARGS;
  }
  return ZX_ERR_NOT_SUPPORTED;
}

static constexpr zx_driver_ops_t driver_ops = []() {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = AmlPwmDevice::Create;
  return ops;
}();

}  // namespace pwm

// clang-format off
ZIRCON_DRIVER_BEGIN(pwm, pwm::driver_ops, "zircon", "0.1", 6)
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_PDEV),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, PDEV_VID_AMLOGIC),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_DID, PDEV_DID_AMLOGIC_PWM),
    // we support multiple SOC variants
    BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_PID, PDEV_PID_AMLOGIC_A113),
    BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_PID, PDEV_PID_AMLOGIC_S905D2),
    BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_PID, PDEV_PID_AMLOGIC_T931),
ZIRCON_DRIVER_END(pwm)
    // clang-format on
