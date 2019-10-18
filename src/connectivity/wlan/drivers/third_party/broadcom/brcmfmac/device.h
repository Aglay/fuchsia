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

#ifndef SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_BROADCOM_BRCMFMAC_DEVICE_H_
#define SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_BROADCOM_BRCMFMAC_DEVICE_H_

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <zircon/types.h>

#include <ddktl/device.h>
#include <ddktl/protocol/wlanphyimpl.h>

#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/core.h"

namespace wlan {
namespace brcmfmac {

using BusRegisterFn = std::function<zx_status_t(brcmf_pub* drvr)>;
class WlanInterface;

class Device : public ::ddk::WlanphyImplProtocol<Device, ::ddk::base_protocol> {
 public:
  virtual ~Device();

  // WlanphyImpl interface implementation.
  zx_status_t WlanphyImplQuery(wlanphy_impl_info_t* out_info);
  zx_status_t WlanphyImplCreateIface(const wlanphy_impl_create_iface_req_t* req,
                                     uint16_t* out_iface_id);
  zx_status_t WlanphyImplDestroyIface(uint16_t iface_id);
  zx_status_t WlanphyImplSetCountry(const wlanphy_country_t* country);

  // Trampolines for DDK functions, for platforms that support them
  virtual zx_status_t DeviceAdd(device_add_args_t* args, zx_device_t** out_device) = 0;
  virtual zx_status_t DeviceRemove(zx_device_t* dev) = 0;

 protected:
  // Initialize the device-agnostic bits of the device
  zx_status_t Init(zx_device_t* phy_device, zx_device_t* parent_device, BusRegisterFn);

  void DisableDispatcher();

 private:
  std::unique_ptr<::async::Loop> dispatcher_;
  std::unique_ptr<brcmf_pub> brcmf_pub_;

  // Two fixed interfaces supported; the default instance as a client, and a second one as an AP.
  WlanInterface* client_interface_;
  WlanInterface* ap_interface_;
};

}  // namespace brcmfmac
}  // namespace wlan

#endif  // SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_BROADCOM_BRCMFMAC_DEVICE_H_
