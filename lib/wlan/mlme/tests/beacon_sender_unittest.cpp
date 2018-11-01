// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <wlan/mlme/ap/beacon_sender.h>
#include <wlan/mlme/ap/bss_interface.h>
#include <wlan/mlme/mac_frame.h>
#include <wlan/mlme/packet.h>
#include <wlan/mlme/service.h>
#include <wlan/mlme/timer.h>

#include <fbl/unique_ptr.h>
#include <fuchsia/wlan/mlme/c/fidl.h>
#include <fuchsia/wlan/mlme/cpp/fidl.h>

#include "mock_device.h"
#include "test_bss.h"

#include <gtest/gtest.h>

namespace wlan {
namespace {

namespace wlan_mlme = ::fuchsia::wlan::mlme;

struct MockBss : public BssInterface {
    const common::MacAddr& bssid() const { return bssid_; }
    uint64_t timestamp() { return 0; }

    seq_t NextSeq(const MgmtFrameHeader& hdr) { return 0; }
    seq_t NextSeq(const MgmtFrameHeader& hdr, uint8_t aci) { return 0; }
    seq_t NextSeq(const DataFrameHeader& hdr) { return 0; }

    std::optional<DataFrame<LlcHeader>> EthToDataFrame(const EthFrame& eth_frame,
                                                       bool needs_protection) {
        return std::nullopt;
    }

    bool IsRsn() const { return false; }
    HtConfig Ht() const { return {}; }
    const Span<const SupportedRate> Rates() const { return {}; }

    zx_status_t SendMgmtFrame(MgmtFrame<>&& mgmt_frame) { return ZX_ERR_NOT_SUPPORTED; }
    zx_status_t SendDataFrame(DataFrame<>&& data_frame) { return ZX_ERR_NOT_SUPPORTED; }
    zx_status_t SendEthFrame(EthFrame&& eth_frame) { return ZX_ERR_NOT_SUPPORTED; }

    void OnPreTbtt() {}
    void OnBcnTxComplete() {}

    wlan_channel_t Chan() const { return {}; }

    common::MacAddr bssid_ = common::MacAddr(kBssid1);
};

struct BeaconSenderTest : public ::testing::Test {
    BeaconSenderTest() : bcn_sender(&device) {}

    void Start() {
        MlmeMsg<wlan_mlme::StartRequest> start_req;
        auto status = CreateStartRequest(&start_req, false);
        ASSERT_EQ(status, ZX_OK);

        bcn_sender.Start(&bss, ps_cfg, start_req);
    }

    MockBss bss;
    MockDevice device;
    BeaconSender bcn_sender;
    PsCfg ps_cfg;
};

TEST_F(BeaconSenderTest, Start) {
    ASSERT_FALSE(device.beaconing_enabled);

    Start();

    ASSERT_TRUE(device.beaconing_enabled);
    ASSERT_EQ(device.beacon.get(), nullptr);

    bcn_sender.UpdateBeacon(ps_cfg);

    auto pkt = fbl::move(device.beacon);
    EXPECT_TRUE(device.beaconing_enabled);
    ASSERT_NE(pkt, nullptr);

    auto checked = MgmtFrameView<Beacon>::CheckType(pkt.get());
    ASSERT_TRUE(checked);
    auto beacon_frame = checked.CheckLength();
    ASSERT_TRUE(beacon_frame);
}

TEST_F(BeaconSenderTest, ProbeRequest) {
    Start();

    ASSERT_TRUE(device.wlan_queue.empty());

    fbl::unique_ptr<Packet> packet;
    CreateProbeRequest(&packet);
    MgmtFrameView<ProbeRequest> probe_req(packet.get());
    bcn_sender.SendProbeResponse(probe_req);

    ASSERT_FALSE(device.wlan_queue.empty());
    auto pkt = std::move(*device.wlan_queue.begin());

    auto checked = MgmtFrameView<ProbeResponse>::CheckType(pkt.get());
    ASSERT_TRUE(checked);
    auto beacon_frame = checked.CheckLength();
    ASSERT_TRUE(beacon_frame);
}

TEST(BeaconSender, ShouldSendProbeResponse) {
    const uint8_t our_ssid[] = {'f', 'o', 'o'};

    const uint8_t no_ssid[] = {1, 1, 1};
    EXPECT_TRUE(ShouldSendProbeResponse(no_ssid, our_ssid));

    const uint8_t different_ssid[] = {0, 3, 'b', 'a', 'r', 1, 1, 1};
    EXPECT_FALSE(ShouldSendProbeResponse(different_ssid, our_ssid));

    const uint8_t matching_ssid[] = {0, 3, 'f', 'o', 'o', 1, 1, 1};
    EXPECT_TRUE(ShouldSendProbeResponse(matching_ssid, our_ssid));

    const uint8_t wildcard_ssid[] = {0, 0, 1, 1, 1};
    EXPECT_TRUE(ShouldSendProbeResponse(wildcard_ssid, our_ssid));

    const uint8_t malformed_ssid[35] = {
        0,
        33,
    };
    EXPECT_FALSE(ShouldSendProbeResponse(malformed_ssid, our_ssid));
}

}  // namespace
}  // namespace wlan
