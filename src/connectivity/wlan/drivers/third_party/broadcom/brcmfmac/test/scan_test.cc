// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gtest/gtest.h>

#include "sim_test.h"
#include "src/connectivity/wlan/drivers/testing/lib/sim-device/device.h"
#include "src/connectivity/wlan/drivers/testing/lib/sim-env/sim-env.h"
#include "src/connectivity/wlan/drivers/testing/lib/sim-fake-ap/sim-fake-ap.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/sim_device.h"

namespace wlan::brcmfmac {

struct ApInfo {
  explicit ApInfo(simulation::Environment* env, const common::MacAddr& bssid,
                  const wlan_ssid_t& ssid, const wlan_channel_t& chan)
      : ap_(env, bssid, ssid, chan) {}

  simulation::FakeAp ap_;
  size_t beacons_seen_count_ = 0;
};

class ScanTest : public SimTest {
 public:
  // These represent notifications we can receive from sim-env.
  struct Notification {
    enum NotificationType { FINISHED, START_SCAN } type;
  };

  static constexpr zx::duration kDefaultTestDuration = zx::sec(100);
  static constexpr zx::duration kScanStartTime = zx::sec(1);
  static constexpr zx::duration kDefaultBeaconInterval = zx::msec(100);

  ScanTest() = default;
  void Init();
  void StartFakeAp(const common::MacAddr& bssid, const wlan_ssid_t& ssid,
                   const wlan_channel_t& chan,
                   zx::duration beacon_interval = kDefaultBeaconInterval);
  void EndSimulation();
  void StartScan();

  enum { NOT_STARTED, RUNNING, COMPLETE } scan_state_ = NOT_STARTED;
  bool all_aps_seen_ = false;

  // SME standin functions
  void OnScanResult(const wlanif_scan_result_t* result);
  void OnScanEnd(const wlanif_scan_end_t* end);

 private:
  // StationIfc methods
  void Rx(void* pkt) override;
  // RxBeacon handler not needed because the test doesn't need to observe them
  void ReceiveNotification(void* payload) override;

  // This is the interface we will use for our single client interface
  std::unique_ptr<SimInterface> client_ifc_;

  // All simulated APs
  std::list<std::unique_ptr<ApInfo>> aps_;

  // Txn ID for the current scan
  uint64_t scan_txn_id_ = 0;

  // SME callbacks
  static wlanif_impl_ifc_protocol_ops_t sme_ops_;
  wlanif_impl_ifc_protocol sme_protocol_ = {.ops = &sme_ops_, .ctx = this};
};

// Since we're acting as wlanif, we need handlers for any protocol calls we may receive
wlanif_impl_ifc_protocol_ops_t ScanTest::sme_ops_ = {
    .on_scan_result =
        [](void* cookie, const wlanif_scan_result_t* result) {
          static_cast<ScanTest*>(cookie)->OnScanResult(result);
        },
    .on_scan_end =
        [](void* cookie, const wlanif_scan_end_t* end) {
          static_cast<ScanTest*>(cookie)->OnScanEnd(end);
        },
};

void ScanTest::Init() {
  ASSERT_EQ(SimTest::Init(), ZX_OK);
  ASSERT_EQ(CreateInterface(WLAN_INFO_MAC_ROLE_CLIENT, sme_protocol_, &client_ifc_), ZX_OK);
}

// Create a new AP with the specified parameters, and tell it to start beaconing
void ScanTest::StartFakeAp(const common::MacAddr& bssid, const wlan_ssid_t& ssid,
                           const wlan_channel_t& chan, zx::duration beacon_interval) {
  auto ap_info = std::make_unique<ApInfo>(env_.get(), bssid, ssid, chan);
  ap_info->ap_.EnableBeacon(beacon_interval);
  aps_.push_back(std::move(ap_info));
}

// Should never be called
void ScanTest::Rx(void* pkt) { GTEST_FAIL(); }

// Called when simulation time has run out. Takes down all fake APs and the simulated DUT.
void ScanTest::EndSimulation() {
  zx_status_t status;
  for (auto ap_info = aps_.begin(); ap_info != aps_.end(); ap_info++) {
    (*ap_info)->ap_.DisableBeacon();
  }
  status = device_->WlanphyImplDestroyIface(client_ifc_->iface_id_);
  // TODO - check status. brcmfmac doesn't support destroying an interface yet.
}

// Tell the DUT to run a scan
void ScanTest::StartScan() {
  auto if_impl_args = &client_ifc_->if_impl_args_;
  auto if_impl_ops = static_cast<wlanif_impl_protocol_ops_t*>(if_impl_args->proto_ops);
  wlanif_scan_req_t req = {
      .txn_id = ++scan_txn_id_,
      .bss_type = WLAN_BSS_TYPE_INFRASTRUCTURE,
      .scan_type = WLAN_SCAN_TYPE_PASSIVE,
      .num_channels = 11,
      .channel_list = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11},
      .num_ssids = 0,
  };
  if_impl_ops->start_scan(if_impl_args->ctx, &req);
  scan_state_ = RUNNING;
}

void ScanTest::ReceiveNotification(void* payload) {
  auto notification = static_cast<Notification*>(payload);
  switch (notification->type) {
    case Notification::FINISHED:
      EndSimulation();
      break;
    case Notification::START_SCAN:
      StartScan();
      break;
    default:
      GTEST_FAIL();
      break;
  }
  delete notification;
}

// Keep track of which AP we received the scan result for, using the BSSID as a unique identifier.
void ScanTest::OnScanResult(const wlanif_scan_result_t* result) {
  int matches_seen = 0;

  ASSERT_NE(result, nullptr);
  EXPECT_EQ(scan_txn_id_, result->txn_id);

  for (auto ap_info = aps_.begin(); ap_info != aps_.end(); ap_info++) {
    common::MacAddr mac_addr = (*ap_info)->ap_.GetBssid();
    ASSERT_EQ(sizeof(result->bss.bssid), sizeof(mac_addr.byte));
    if (!std::memcmp(result->bss.bssid, mac_addr.byte, sizeof(mac_addr.byte))) {
      (*ap_info)->beacons_seen_count_++;
      matches_seen++;

      // Verify SSID
      wlan_ssid_t ssid_info = (*ap_info)->ap_.GetSsid();
      EXPECT_EQ(result->bss.ssid.len, ssid_info.len);
      ASSERT_LE(ssid_info.len, sizeof(ssid_info.ssid));
      EXPECT_EQ(memcmp(result->bss.ssid.data, ssid_info.ssid, ssid_info.len), 0);

      wlan_channel_t channel = (*ap_info)->ap_.GetChannel();
      EXPECT_EQ(result->bss.chan.primary, channel.primary);
      EXPECT_EQ(result->bss.chan.cbw, channel.cbw);
      EXPECT_EQ(result->bss.chan.secondary80, channel.secondary80);
    }
  }

  // There should be exactly one AP per result.
  EXPECT_EQ(matches_seen, 1);
}

void ScanTest::OnScanEnd(const wlanif_scan_end_t* end) {
  scan_state_ = COMPLETE;

  for (auto ap_info = aps_.begin(); ap_info != aps_.end(); ap_info++) {
    if ((*ap_info)->beacons_seen_count_ == 0) {
      // Failure
      return;
    }
  }

  // The beacons from all APs were seen
  all_aps_seen_ = true;
}

constexpr wlan_channel_t kDefaultChannel = {
    .primary = 9, .cbw = WLAN_CHANNEL_BANDWIDTH__20, .secondary80 = 0};
constexpr wlan_ssid_t kDefaultSsid = {.ssid = "Fuchsia Fake AP", .len = 15};
const common::MacAddr kDefaultBssid({0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc});

TEST_F(ScanTest, BasicFunctionality) {
  // Create our simulated device
  Init();

  // Start up a single AP
  StartFakeAp(kDefaultBssid, kDefaultSsid, kDefaultChannel);

  // Request a future scan
  Notification* notification = new Notification;
  notification->type = Notification::START_SCAN;
  env_->ScheduleNotification(this, kScanStartTime, notification);

  // Request a future notification so we can shut down the test
  notification = new Notification;
  notification->type = Notification::FINISHED;
  env_->ScheduleNotification(this, kDefaultTestDuration, notification);

  env_->Run();

  EXPECT_EQ(all_aps_seen_, true);
}

}  // namespace wlan::brcmfmac
