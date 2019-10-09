// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "ti-lp8556.h"

#include <lib/device-protocol/i2c.h>
#include <lib/device-protocol/pdev.h>

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/metadata.h>
#include <ddk/platform-defs.h>
#include <ddk/protocol/composite.h>
#include <ddktl/fidl.h>
#include <fbl/algorithm.h>
#include <fbl/alloc_checker.h>
#include <fbl/unique_ptr.h>

namespace ti {

enum {
  COMPONENT_PDEV,
  COMPONENT_I2C,
  COMPONENT_COUNT,
};

void Lp8556Device::DdkUnbindNew(ddk::UnbindTxn txn) { txn.Reply(); }

void Lp8556Device::DdkRelease() { delete this; }

zx_status_t Lp8556Device::GetBacklightState(bool* power, double* brightness) {
  *power = power_;
  *brightness = brightness_;
  return ZX_OK;
}

zx_status_t Lp8556Device::SetBacklightState(bool power, double brightness) {
  brightness = fbl::max(brightness, 0.0);
  brightness = fbl::min(brightness, 1.0);

  if (brightness != brightness_) {
    uint8_t buf[2];
    buf[0] = kBacklightControlReg;
    buf[1] = static_cast<uint8_t>(brightness * kMaxBrightnessRegValue);
    zx_status_t status = i2c_.WriteSync(buf, sizeof(buf));
    if (status != ZX_OK) {
      LOG_ERROR("Failed to set brightness register\n");
      return status;
    }

    uint16_t sticky_reg_value = static_cast<uint16_t>(brightness * kAOBrightnessStickyMaxValue);
    sticky_reg_value &= kAOBrightnessStickyMask;

    auto persistent_brightness = BrightnessStickyReg::Get().ReadFrom(&mmio_);
    persistent_brightness.set_brightness(sticky_reg_value);
    persistent_brightness.set_is_valid(1);
    persistent_brightness.WriteTo(&mmio_);
  }

  if (power != power_) {
    uint8_t buf[2];
    buf[0] = kDeviceControlReg;
    buf[1] = power ? kBacklightOn : kBacklightOff;
    zx_status_t status = i2c_.WriteSync(buf, sizeof(buf));
    if (status != ZX_OK) {
      LOG_ERROR("Failed to set device control register\n");
      return status;
    }

    if (power) {
      buf[0] = kCfg2Reg;
      buf[1] = cfg2_;
      status = i2c_.WriteSync(buf, sizeof(buf));
      if (status != ZX_OK) {
        LOG_ERROR("Failed to set cfg2 register\n");
        return status;
      }
    }
  }

  // update internal values
  power_ = power;
  brightness_ = brightness;
  return ZX_OK;
}

void Lp8556Device::GetStateNormalized(GetStateNormalizedCompleter::Sync completer) {
  FidlBacklight::State state = {};
  auto status = GetBacklightState(&state.backlight_on, &state.brightness);

  FidlBacklight::Device_GetStateNormalized_Result result;
  if (status == ZX_OK) {
    result.set_response(FidlBacklight::Device_GetStateNormalized_Response{.state = state});
  } else {
    result.set_err(status);
  }
  completer.Reply(std::move(result));
}

void Lp8556Device::SetStateNormalized(FidlBacklight::State state,
                                      SetStateNormalizedCompleter::Sync completer) {
  auto status = SetBacklightState(state.backlight_on, state.brightness);

  FidlBacklight::Device_SetStateNormalized_Result result;
  if (status == ZX_OK) {
    result.set_response(FidlBacklight::Device_SetStateNormalized_Response{});
  } else {
    result.set_err(status);
  }
  completer.Reply(std::move(result));
}

void Lp8556Device::GetStateAbsolute(GetStateAbsoluteCompleter::Sync completer) {
  FidlBacklight::Device_GetStateAbsolute_Result result;
  if (!max_absolute_brightness_nits_.has_value()) {
    result.set_err(ZX_ERR_NOT_SUPPORTED);
    completer.Reply(std::move(result));
    return;
  }

  FidlBacklight::State state = {};
  auto status = GetBacklightState(&state.backlight_on, &state.brightness);
  if (status == ZX_OK) {
    state.brightness *= max_absolute_brightness_nits_.value();
    result.set_response(FidlBacklight::Device_GetStateAbsolute_Response{.state = state});
  } else {
    result.set_err(status);
  }
  completer.Reply(std::move(result));
}

void Lp8556Device::SetStateAbsolute(FidlBacklight::State state,
                                    SetStateAbsoluteCompleter::Sync completer) {
  FidlBacklight::Device_SetStateAbsolute_Result result;
  if (!max_absolute_brightness_nits_.has_value()) {
    result.set_err(ZX_ERR_NOT_SUPPORTED);
    completer.Reply(std::move(result));
    return;
  }

  auto status = SetBacklightState(state.backlight_on,
                                  state.brightness / max_absolute_brightness_nits_.value());
  if (status == ZX_OK) {
    result.set_response(FidlBacklight::Device_SetStateAbsolute_Response{});
  } else {
    result.set_err(status);
  }
  completer.Reply(std::move(result));
}

void Lp8556Device::GetMaxAbsoluteBrightness(GetMaxAbsoluteBrightnessCompleter::Sync completer) {
  FidlBacklight::Device_GetMaxAbsoluteBrightness_Result result;
  if (max_absolute_brightness_nits_.has_value()) {
    result.set_response(FidlBacklight::Device_GetMaxAbsoluteBrightness_Response{
        .max_brightness = max_absolute_brightness_nits_.value()});
  } else {
    result.set_err(ZX_ERR_NOT_SUPPORTED);
  }
  completer.Reply(std::move(result));
}

zx_status_t Lp8556Device::DdkMessage(fidl_msg_t* msg, fidl_txn_t* txn) {
  DdkTransaction transaction(txn);
  FidlBacklight::Device::Dispatch(this, msg, &transaction);
  return transaction.Status();
}

zx_status_t ti_lp8556_bind(void* ctx, zx_device_t* parent) {
  composite_protocol_t composite;

  auto status = device_get_protocol(parent, ZX_PROTOCOL_COMPOSITE, &composite);
  if (status != ZX_OK) {
    LOG_ERROR("Could not get composite protocol\n");
    return status;
  }

  zx_device_t* components[COMPONENT_COUNT];
  size_t actual;
  composite_get_components(&composite, components, COMPONENT_COUNT, &actual);
  if (actual != COMPONENT_COUNT) {
    LOG_ERROR("Could not get components\n");
    return ZX_ERR_INTERNAL;
  }

  // Get platform device protocol
  ddk::PDev pdev(components[COMPONENT_PDEV]);
  if (!pdev.is_valid()) {
    LOG_ERROR("Could not get PDEV protocol\n");
    return ZX_ERR_NO_RESOURCES;
  }

  // Map MMIO
  std::optional<ddk::MmioBuffer> mmio;
  status = pdev.MapMmio(0, &mmio);
  if (status != ZX_OK) {
    LOG_ERROR("Could not map mmio %d\n", status);
    return status;
  }

  // Obtain I2C protocol needed to control backlight
  i2c_protocol_t i2c;
  status = device_get_protocol(components[COMPONENT_I2C], ZX_PROTOCOL_I2C, &i2c);
  if (status != ZX_OK) {
    LOG_ERROR("Could not obtain I2C protocol\n");
    return status;
  }
  ddk::I2cChannel i2c_channel(&i2c);

  fbl::AllocChecker ac;
  auto dev = fbl::make_unique_checked<ti::Lp8556Device>(&ac, parent, std::move(i2c_channel),
                                                        *std::move(mmio));
  if (!ac.check()) {
    return ZX_ERR_NO_MEMORY;
  }

  double brightness_nits = 0.0;
  status = device_get_metadata(parent, DEVICE_METADATA_BACKLIGHT_MAX_BRIGHTNESS_NITS,
                               &brightness_nits, sizeof(brightness_nits), &actual);
  if (status == ZX_OK && actual == sizeof(brightness_nits)) {
    dev->SetMaxAbsoluteBrightnessNits(brightness_nits);
  }

  status = dev->DdkAdd("ti-lp8556");
  if (status != ZX_OK) {
    LOG_ERROR("Could not add device\n");
    return status;
  }

  // devmgr is now in charge of memory for dev
  __UNUSED auto ptr = dev.release();

  return status;
}

static constexpr zx_driver_ops_t ti_lp8556_driver_ops = []() {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = ti_lp8556_bind;
  return ops;
}();

}  // namespace ti

// clang-format off
ZIRCON_DRIVER_BEGIN(ti_lp8556, ti::ti_lp8556_driver_ops, "TI-LP8556", "0.1", 4)
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_COMPOSITE),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, PDEV_VID_TI),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_PID, PDEV_PID_TI_LP8556),
    BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_DID, PDEV_DID_TI_BACKLIGHT),
ZIRCON_DRIVER_END(ti_lp8556)
    // clang-format on
