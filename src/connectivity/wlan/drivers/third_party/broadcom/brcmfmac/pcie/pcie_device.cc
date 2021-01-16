// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/pcie/pcie_device.h"

#include <zircon/errors.h>
#include <zircon/status.h>

#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/bus.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/core.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/debug.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/msgbuf/msgbuf_proto.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/pcie/pcie_bus.h"

namespace wlan {
namespace brcmfmac {

PcieDevice::PcieDevice(zx_device_t* parent) : Device(parent) {}

PcieDevice::~PcieDevice() {
  brcmf_detach(drvr());
  Stop();
}

// static
zx_status_t PcieDevice::Create(zx_device_t* parent_device) {
  zx_status_t status = ZX_OK;

  const auto ddk_remover = [](PcieDevice* device) { device->DdkAsyncRemove(); };
  std::unique_ptr<PcieDevice, decltype(ddk_remover)> device(new PcieDevice(parent_device),
                                                            ddk_remover);
  if ((status = device->DdkAdd(ddk::DeviceAddArgs("brcmfmac-wlanphy")
                                   .set_flags(DEVICE_ADD_INVISIBLE)
                                   .set_inspect_vmo(device->inspect_.GetVmo()))) != ZX_OK) {
    delete device.release();
    return status;
  }

  if ((status = device->brcmfmac::Device::Init()) != ZX_OK) {
    return status;
  }

  std::unique_ptr<PcieBus> pcie_bus;
  if ((status = PcieBus::Create(device.get(), &pcie_bus)) != ZX_OK) {
    return status;
  }

  std::unique_ptr<MsgbufProto> msgbuf_proto;
  if ((status = MsgbufProto::Create(device.get(), pcie_bus->GetDmaBufferProvider(),
                                    pcie_bus->GetDmaRingProvider(),
                                    pcie_bus->GetInterruptProvider(), &msgbuf_proto)) != ZX_OK) {
    return status;
  }

  device->pcie_bus_ = std::move(pcie_bus);
  device->msgbuf_proto_ = std::move(msgbuf_proto);

  if ((status = brcmf_attach(device->drvr())) != ZX_OK) {
    BRCMF_ERR("Failed to attach: %s", zx_status_get_string(status));
    return status;
  }

  if ((status = brcmf_bus_started(device->drvr())) != ZX_OK) {
    BRCMF_ERR("Failed to start bus: %s", zx_status_get_string(status));
    return status;
  }

  if ((status = device->brcmfmac::Device::Start()) != ZX_OK) {
    return status;
  }

  // TODO(sheu): make the device visible once higher-level functionality is present.
  // device->DdkMakeVisible();

  device.release();  // This now has its lifecycle managed by the devhost.
  return ZX_OK;
}

zx_status_t PcieDevice::DeviceAdd(device_add_args_t* args, zx_device_t** out_device) {
  return device_add(zxdev(), args, out_device);
}

void PcieDevice::DeviceAsyncRemove(zx_device_t* dev) { device_async_remove(dev); }

zx_status_t PcieDevice::LoadFirmware(const char* path, zx_handle_t* fw, size_t* size) {
  return load_firmware(zxdev(), path, fw, size);
}

zx_status_t PcieDevice::DeviceGetMetadata(uint32_t type, void* buf, size_t buflen, size_t* actual) {
  return device_get_metadata(zxdev(), type, buf, buflen, actual);
}

}  // namespace brcmfmac
}  // namespace wlan
