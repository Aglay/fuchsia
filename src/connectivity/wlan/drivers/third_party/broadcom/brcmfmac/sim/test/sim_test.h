// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_BROADCOM_BRCMFMAC_SIM_TEST_SIM_TEST_H_
#define SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_BROADCOM_BRCMFMAC_SIM_TEST_SIM_TEST_H_

#include <zircon/types.h>

#include <gtest/gtest.h>

#include "src/connectivity/wlan/drivers/testing/lib/sim-device/device.h"
#include "src/connectivity/wlan/drivers/testing/lib/sim-env/sim-env.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/sim/sim_device.h"

namespace wlan::brcmfmac {

// This class represents an interface created on a simulated device, collecting all of the
// attributes related to that interface.
struct SimInterface {
  SimInterface() = default;

  zx_status_t Init() { return zx_channel_create(0, &ch_sme_, &ch_mlme_); }

  virtual ~SimInterface() {
    if (ch_sme_ != ZX_HANDLE_INVALID) {
      zx_handle_close(ch_sme_);
    }
    if (ch_mlme_ != ZX_HANDLE_INVALID) {
      zx_handle_close(ch_mlme_);
    }
  }

  // Default SME Callbacks
  virtual void OnScanResult(const wlanif_scan_result_t* result) {}
  virtual void OnScanEnd(const wlanif_scan_end_t* end) {}
  virtual void OnJoinConf(const wlanif_join_confirm_t* resp) {}
  virtual void OnAuthConf(const wlanif_auth_confirm_t* resp) {}
  virtual void OnAuthInd(const wlanif_auth_ind_t* resp) {}
  virtual void OnDeauthConf(const wlanif_deauth_confirm_t* resp) {}
  virtual void OnDeauthInd(const wlanif_deauth_indication_t* ind) {}
  virtual void OnAssocConf(const wlanif_assoc_confirm_t* resp) {}
  virtual void OnAssocInd(const wlanif_assoc_ind_t* ind) {}
  virtual void OnDisassocConf(const wlanif_disassoc_confirm_t* resp) {}
  virtual void OnDisassocInd(const wlanif_disassoc_indication_t* ind) {}
  virtual void OnStartConf(const wlanif_start_confirm_t* resp) {}
  virtual void OnStopConf(const wlanif_stop_confirm_t* resp) {}
  virtual void OnEapolConf(const wlanif_eapol_confirm_t* resp) {}
  virtual void OnChannelSwitch(const wlanif_channel_switch_info_t* ind) {}
  virtual void OnSignalReport(const wlanif_signal_report_indication_t* ind) {}
  virtual void OnEapolInd(const wlanif_eapol_indication_t* ind) {}
  virtual void OnStatsQueryResp(const wlanif_stats_query_response_t* resp) {}
  virtual void OnRelayCapturedFrame(const wlanif_captured_frame_result_t* result) {}
  virtual void OnDataRecv(const void* data, size_t data_size, uint32_t flags) {}

  // Default protocols that redirect to the above virtual functions
  static wlanif_impl_ifc_protocol_ops_t default_sme_dispatch_tbl_;
  wlanif_impl_ifc_protocol default_ifc_ = {.ops = &default_sme_dispatch_tbl_, .ctx = this};

  // This provides our DDK (wlanif-impl) API into the interface
  void* if_impl_ctx_;
  wlanif_impl_protocol_ops_t* if_impl_ops_;

  // Unique identifier provided by the driver
  uint16_t iface_id_;

  // Handles for SME <=> MLME communication, required but never used for communication (since no
  // SME is present).
  zx_handle_t ch_sme_ = ZX_HANDLE_INVALID;   // SME-owned side
  zx_handle_t ch_mlme_ = ZX_HANDLE_INVALID;  // MLME-owned side
};

// A base class that can be used for creating simulation tests. It provides functionality that
// should be common to most tests (like creating a new device instance and setting up and plugging
// into the environment). It also provides a factory method for creating a new interface on the
// simulated device.
class SimTest : public ::testing::Test, public simulation::StationIfc {
 public:
  SimTest();
  zx_status_t Init();

  std::shared_ptr<simulation::Environment> env_;

  static intptr_t instance_num_;

 protected:
  // Create a new interface on the simulated device, providing the specified role and function
  // callbacks
  zx_status_t StartInterface(wlan_info_mac_role_t role, SimInterface* sim_ifc,
                             std::optional<const wlanif_impl_ifc_protocol*> sme_protocol,
                             std::optional<common::MacAddr> mac_addr = std::nullopt);

  // Fake device manager
  std::shared_ptr<simulation::FakeDevMgr> dev_mgr_;

  // brcmfmac's concept of a device
  std::unique_ptr<brcmfmac::SimDevice> device_;

 private:
  // StationIfc methods - by default, do nothing. These can/will be overridden by superclasses.
  void Rx(std::shared_ptr<const simulation::SimFrame> frame,
          std::shared_ptr<const simulation::WlanRxInfo> info) override {}

  // Contrived pointer used as a stand-in for the (opaque) parent device
  zx_device_t* parent_dev_;
};

}  // namespace wlan::brcmfmac

#endif  // SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_BROADCOM_BRCMFMAC_SIM_TEST_SIM_TEST_H_
