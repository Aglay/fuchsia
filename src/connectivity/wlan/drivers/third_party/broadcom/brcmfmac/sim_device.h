// Copyright (c) 2019 The Fuchsia Authors
//
// Permission to use, copy, modify, and/or distribute this software for any purpose with or without
// fee is hereby granted, provided that the above copyright notice and this permission notice
// appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS
// SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
// AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
// NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
// OF THIS SOFTWARE.

#ifndef SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_BROADCOM_BRCMFMAC_SIM_DEVICE_H_
#define SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_BROADCOM_BRCMFMAC_SIM_DEVICE_H_

#include "src/connectivity/wlan/drivers/testing/lib/sim-device/device.h"
#include "src/connectivity/wlan/drivers/testing/lib/sim-env/sim-env.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/bus.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/device.h"

namespace wlan::brcmfmac {

class SimDevice : public Device {
 public:
  ~SimDevice();
  // Static factory function for SimDevice instances.
  static zx_status_t Create(zx_device_t* parent_device,
                            const std::shared_ptr<simulation::FakeDevMgr>& dev_mgr,
                            const std::shared_ptr<simulation::Environment>& env,
                            std::unique_ptr<SimDevice>* device_out);

  explicit SimDevice(zx_device_t* phy_device,
                     const std::shared_ptr<simulation::FakeDevMgr>& dev_mgr,
                     const std::shared_ptr<simulation::Environment>& env)
      : phy_device_(phy_device), fake_dev_mgr_(dev_mgr), sim_environ_(env) {}

  SimDevice(const SimDevice& device) = delete;
  SimDevice& operator=(const SimDevice& other) = delete;

  // Trampolines for DDK functions, for platforms that support them.
  zx_status_t DeviceAdd(device_add_args_t* args, zx_device_t** out_device) override;
  zx_status_t DeviceRemove(zx_device_t* dev) override;
  zx_status_t LoadFirmware(const char* path, zx_handle_t* fw, size_t* size) override;

 private:
  std::unique_ptr<brcmf_bus> brcmf_bus_;
  zx_device_t* phy_device_;
  std::shared_ptr<simulation::FakeDevMgr> fake_dev_mgr_;
  std::shared_ptr<simulation::Environment> sim_environ_;
};

}  // namespace wlan::brcmfmac

#endif  // SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_BROADCOM_BRCMFMAC_SIM_DEVICE_H_
