// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/wlan/mlme/cpp/fidl.h>
#include <lib/timekeeper/clock.h>

#include <gtest/gtest.h>
#include <wlan/common/buffer_writer.h>
#include <wlan/common/element_splitter.h>
#include <wlan/common/write_element.h>
#include <wlan/mlme/ap/tim.h>
#include <wlan/mlme/client/client_mlme.h>
#include <wlan/mlme/mac_frame.h>
#include <wlan/mlme/packet.h>
#include <wlan/mlme/service.h>
#include <wlan/mlme/timer.h>
#include <wlan/mlme/validate_frame.h>

#include "mock_device.h"
#include "test_bss.h"
#include "test_utils.h"

namespace wlan {

namespace {

namespace wlan_mlme = ::fuchsia::wlan::mlme;

constexpr uint8_t kTestPayload[] = "Hello Fuchsia";

wlan_client_mlme_config_t ClientTestConfig() {
  return wlan_client_mlme_config_t{
      // Set to a really high value to prevent SignalReport msg from being sent since most
      // of the time we don't really care about it in our test.
      .signal_report_beacon_timeout = 9999999,
      // Set to 0 to more easily control the timing for going on- and off-channel so that
      // auto-deauth tests are simpler.
      .ensure_on_channel_time = 0,
  };
}

struct ClientTest : public ::testing::Test {
  ClientTest() : device(), client(&device, ClientTestConfig()) {}

  void SetUp() override {
    device.SetTime(zx::time(0));
    client.Init();
    TriggerTimeout();
  }

  zx_status_t SendNullDataFrame() {
    auto frame = CreateNullDataFrame();
    if (frame.IsEmpty()) {
      return ZX_ERR_NO_RESOURCES;
    }
    client.HandleFramePacket(frame.Take());
    return ZX_OK;
  }

  void SendBeaconFrame(const common::MacAddr& bssid = common::MacAddr(kBssid1)) {
    client.HandleFramePacket(CreateBeaconFrame(bssid));
  }

  void TriggerTimeout() {
    ObjectId timer_id;
    timer_id.set_subtype(to_enum_type(ObjectSubtype::kTimer));
    timer_id.set_target(to_enum_type(ObjectTarget::kClientMlme));
    client.HandleTimeout(timer_id);
  }

  void Join(bool rsne = true) {
    ASSERT_EQ(ZX_OK, client.HandleMlmeMsg(CreateJoinRequest(rsne)));
    device.svc_queue.clear();
  }

  void Authenticate() {
    client.HandleMlmeMsg(CreateAuthRequest());
    client.HandleFramePacket(CreateAuthRespFrame(AuthAlgorithm::kOpenSystem));
    device.svc_queue.clear();
    device.wlan_queue.clear();
    TriggerTimeout();
  }

  void Associate(bool rsne = true) {
    client.HandleMlmeMsg(CreateAssocRequest(rsne));
    client.HandleFramePacket(CreateAssocRespFrame());
    device.svc_queue.clear();
    device.wlan_queue.clear();
    TriggerTimeout();
  }

  void SetKey() {
    auto key_data = std::vector(std::cbegin(kKeyData), std::cend(kKeyData));
    client.HandleMlmeMsg(
        CreateSetKeysRequest(common::MacAddr(kBssid1), key_data, wlan_mlme::KeyType::PAIRWISE));
  }

  void EstablishRsna() {
    client.HandleMlmeMsg(
        CreateSetCtrlPortRequest(common::MacAddr(kBssid1), wlan_mlme::ControlledPortState::OPEN));
  }

  void Connect(bool rsne = true) {
    Join(rsne);
    Authenticate();
    Associate(rsne);
    if (rsne) {
      EstablishRsna();
    }
    // Clear any existing ensure-on-channel flag.
    TriggerTimeout();
  }

  zx::duration BeaconPeriodsToDuration(size_t periods) {
    return zx::usec(1024) * (periods * kBeaconPeriodTu);
  }

  void SetTimeInBeaconPeriods(size_t periods) {
    device.SetTime(zx::time(0) + BeaconPeriodsToDuration(periods));
  }

  void IncreaseTimeByBeaconPeriods(size_t periods) {
    device.SetTime(device.GetTime() + BeaconPeriodsToDuration(periods));
  }

  // Go off channel. This assumes that any existing ensure-on-channel flag is already cleared
  void GoOffChannel(uint16_t beacon_periods) {
    // For our test, scan duration doesn't matter for now since we explicit
    // force station to go back on channel by calling `HandleTimeout`
    ASSERT_EQ(ZX_OK, client.HandleMlmeMsg(CreateScanRequest(beacon_periods * kBeaconPeriodTu)));
    ASSERT_FALSE(client.OnChannel());                    // sanity check
    device.wlan_queue.erase(device.wlan_queue.begin());  // dequeue power-saving frame
  }

  // Trigger timeout to go on channel. This assumes that current off-channel time is
  // exhausted.
  void TriggerTimeoutToGoOnChannel() {
    TriggerTimeout();
    ASSERT_TRUE(client.OnChannel());  // sanity check
    ASSERT_EQ(device.wlan_queue.size(), static_cast<size_t>(1));
    device.wlan_queue.erase(device.wlan_queue.begin());  // dequeue power-saving frame
  }

  void AssertAuthConfirm(MlmeMsg<wlan_mlme::AuthenticateConfirm> msg,
                         wlan_mlme::AuthenticateResultCodes result_code) {
    EXPECT_EQ(msg.body()->result_code, result_code);
  }

  void AssertAssocConfirm(MlmeMsg<wlan_mlme::AssociateConfirm> msg, uint16_t aid,
                          wlan_mlme::AssociateResultCodes result_code) {
    EXPECT_EQ(msg.body()->association_id, aid);
    EXPECT_EQ(msg.body()->result_code, result_code);
  }

  void AssertAuthFrame(WlanPacket pkt) {
    auto frame = TypeCheckWlanFrame<MgmtFrameView<Authentication>>(pkt.pkt.get());
    EXPECT_EQ(std::memcmp(frame.hdr()->addr1.byte, kBssid1, 6), 0);
    EXPECT_EQ(std::memcmp(frame.hdr()->addr2.byte, kClientAddress, 6), 0);
    EXPECT_EQ(std::memcmp(frame.hdr()->addr3.byte, kBssid1, 6), 0);
    EXPECT_EQ(frame.body()->auth_algorithm_number, AuthAlgorithm::kOpenSystem);
    EXPECT_EQ(frame.body()->auth_txn_seq_number, 1);
    EXPECT_EQ(frame.body()->status_code, 0);
  }

  void AssertDeauthFrame(WlanPacket pkt, wlan_mlme::ReasonCode reason_code) {
    auto frame = TypeCheckWlanFrame<MgmtFrameView<Deauthentication>>(pkt.pkt.get());
    EXPECT_EQ(std::memcmp(frame.hdr()->addr1.byte, kBssid1, 6), 0);
    EXPECT_EQ(std::memcmp(frame.hdr()->addr2.byte, kClientAddress, 6), 0);
    EXPECT_EQ(std::memcmp(frame.hdr()->addr3.byte, kBssid1, 6), 0);
    EXPECT_EQ(frame.body()->reason_code, static_cast<uint16_t>(reason_code));
  }

  void AssertAssocReqFrame(WlanPacket pkt, bool rsne) {
    auto frame = TypeCheckWlanFrame<MgmtFrameView<AssociationRequest>>(pkt.pkt.get());
    EXPECT_EQ(std::memcmp(frame.hdr()->addr1.byte, kBssid1, 6), 0);
    EXPECT_EQ(std::memcmp(frame.hdr()->addr2.byte, kClientAddress, 6), 0);
    EXPECT_EQ(std::memcmp(frame.hdr()->addr3.byte, kBssid1, 6), 0);
    auto assoc_req_frame = frame.NextFrame();
    fbl::Span<const uint8_t> ie_chain{assoc_req_frame.body_data()};
    ASSERT_TRUE(ValidateFrame("invalid assoc request", *pkt.pkt));

    bool has_ssid = false;
    bool has_rsne = false;
    for (auto [id, body] : common::ElementSplitter(ie_chain)) {
      if (id == element_id::kSsid) {
        has_ssid = true;
      } else if (id == element_id::kRsn) {
        has_rsne = true;
        if (rsne) {
          // kRsne contains two bytes for element ID and length; the rest are
          // RSNE bytes
          EXPECT_EQ(std::memcmp(body.data(), kRsne + 2, body.size()), 0);
          EXPECT_EQ(body.size(), sizeof(kRsne) - 2);
        }
      }
    }
    EXPECT_TRUE(has_ssid);
    EXPECT_EQ(has_rsne, rsne);
  }

  void AssertKeepAliveFrame(WlanPacket pkt) {
    auto data_frame = TypeCheckWlanFrame<DataFrameView<>>(pkt.pkt.get());
    EXPECT_EQ(data_frame.hdr()->fc.to_ds(), 1);
    EXPECT_EQ(data_frame.hdr()->fc.from_ds(), 0);
    EXPECT_EQ(std::memcmp(data_frame.hdr()->addr1.byte, kBssid1, 6), 0);
    EXPECT_EQ(std::memcmp(data_frame.hdr()->addr2.byte, kClientAddress, 6), 0);
    EXPECT_EQ(std::memcmp(data_frame.hdr()->addr3.byte, kBssid1, 6), 0);
    EXPECT_EQ(data_frame.body_len(), static_cast<size_t>(0));
  }

  struct DataFrameAssert {
    unsigned char protected_frame = 0;
    unsigned char more_data = 0;
  };

  void AssertDataFrameSentToAp(WlanPacket pkt, fbl::Span<const uint8_t> expected_payload,
                               DataFrameAssert asserts = {.protected_frame = 0, .more_data = 0}) {
    auto frame = TypeCheckWlanFrame<DataFrameView<LlcHeader>>(pkt.pkt.get());
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame.hdr()->fc.more_data(), asserts.more_data);
    EXPECT_EQ(std::memcmp(frame.hdr()->addr1.byte, kBssid1, 6), 0);
    EXPECT_EQ(std::memcmp(frame.hdr()->addr2.byte, kClientAddress, 6), 0);
    EXPECT_EQ(std::memcmp(frame.hdr()->addr3.byte, kBssid1, 6), 0);
    EXPECT_EQ(frame.hdr()->fc.protected_frame(), asserts.protected_frame);

    auto llc_frame = frame.NextFrame();
    EXPECT_RANGES_EQ(llc_frame.body_data(), expected_payload);
  }

  MockDevice device;
  ClientMlme client;
};

TEST_F(ClientTest, Join) {
  // (sme->mlme) Send JOIN.request. Verify a JOIN.confirm message was then sent
  // to SME.
  ASSERT_EQ(ZX_OK, client.HandleMlmeMsg(CreateJoinRequest(true)));
  ASSERT_EQ(device.svc_queue.size(), static_cast<size_t>(1));
  auto joins = device.GetServiceMsgs<wlan_mlme::JoinConfirm>(
      fuchsia::wlan::mlme::internal::kMLME_JoinConf_GenOrdinal);
  ASSERT_EQ(joins.size(), 1ULL);
  ASSERT_EQ(joins[0].body()->result_code, wlan_mlme::JoinResultCodes::SUCCESS);
}

TEST_F(ClientTest, Authenticate) {
  Join();

  // (sme->mlme) Send AUTHENTICATION.request. Verify that no confirmation was
  // sent yet.
  ASSERT_EQ(ZX_OK, client.HandleMlmeMsg(CreateAuthRequest()));
  ASSERT_TRUE(device.svc_queue.empty());

  // Verify wlan frame sent to AP is correct.
  ASSERT_EQ(device.wlan_queue.size(), static_cast<size_t>(1));
  AssertAuthFrame(std::move(*device.wlan_queue.begin()));

  // (ap->mlme) Respond with a Authentication frame. Verify a
  // AUTHENTICATION.confirm message was
  //            then sent to SME
  ASSERT_EQ(ZX_OK, client.HandleFramePacket(CreateAuthRespFrame(AuthAlgorithm::kOpenSystem)));
  ASSERT_EQ(device.svc_queue.size(), static_cast<size_t>(1));
  auto auths = device.GetServiceMsgs<wlan_mlme::AuthenticateConfirm>(
      fuchsia::wlan::mlme::internal::kMLME_AuthenticateConf_GenOrdinal);
  ASSERT_EQ(auths.size(), 1ULL);
  AssertAuthConfirm(std::move(auths[0]), wlan_mlme::AuthenticateResultCodes::SUCCESS);

  // Verify a delayed timeout won't cause another confirmation.
  device.svc_queue.clear();
  SetTimeInBeaconPeriods(100);
  TriggerTimeout();
  ASSERT_TRUE(device.svc_queue.empty());
}

TEST_F(ClientTest, Associate_Protected) {
  Join();
  Authenticate();

  // (sme->mlme) Send ASSOCIATE.request. Verify that no confirmation was sent
  // yet.
  ASSERT_EQ(ZX_OK, client.HandleMlmeMsg(CreateAssocRequest(true)));
  ASSERT_TRUE(device.svc_queue.empty());

  // Verify wlan frame sent to AP is correct.
  ASSERT_EQ(device.wlan_queue.size(), static_cast<size_t>(1));
  AssertAssocReqFrame(std::move(*device.wlan_queue.begin()), true);

  // (ap->mlme) Respond with a Association Response frame. Verify a
  // ASSOCIATE.confirm message was
  //            then sent to SME.
  ASSERT_EQ(ZX_OK, client.HandleFramePacket(CreateAssocRespFrame()));
  ASSERT_FALSE(device.svc_queue.empty());
  auto assocs = device.GetServiceMsgs<wlan_mlme::AssociateConfirm>(
      fuchsia::wlan::mlme::internal::kMLME_AssociateConf_GenOrdinal);
  ASSERT_EQ(assocs.size(), 1ULL);
  AssertAssocConfirm(std::move(assocs[0]), kAid, wlan_mlme::AssociateResultCodes::SUCCESS);

  // Verify a delayed timeout won't cause another confirmation.
  device.svc_queue.clear();
  SetTimeInBeaconPeriods(100);
  TriggerTimeout();
  assocs = device.GetServiceMsgs<wlan_mlme::AssociateConfirm>(
      fuchsia::wlan::mlme::internal::kMLME_AssociateConf_GenOrdinal);
  ASSERT_EQ(assocs.size(), 0ULL);
}

TEST_F(ClientTest, Associate_Unprotected) {
  // (sme->mlme) Send JOIN.request. Verify a JOIN.confirm message was then sent
  // to SME.
  ASSERT_EQ(ZX_OK, client.HandleMlmeMsg(CreateJoinRequest(false)));
  ASSERT_EQ(device.svc_queue.size(), static_cast<size_t>(1));
  auto joins = device.GetServiceMsgs<wlan_mlme::JoinConfirm>(
      fuchsia::wlan::mlme::internal::kMLME_JoinConf_GenOrdinal);
  ASSERT_EQ(joins.size(), 1ULL);
  ASSERT_EQ(joins[0].body()->result_code, wlan_mlme::JoinResultCodes::SUCCESS);

  // (sme->mlme) Send AUTHENTICATION.request. Verify that no confirmation was
  // sent yet.
  ASSERT_EQ(ZX_OK, client.HandleMlmeMsg(CreateAuthRequest()));
  ASSERT_TRUE(device.svc_queue.empty());

  // Verify wlan frame sent to AP is correct.
  ASSERT_EQ(device.wlan_queue.size(), static_cast<size_t>(1));
  AssertAuthFrame(std::move(*device.wlan_queue.begin()));
  device.wlan_queue.clear();

  // (ap->mlme) Respond with a Authentication frame. Verify a
  // AUTHENTICATION.confirm message was
  //            then sent to SME
  ASSERT_EQ(ZX_OK, client.HandleFramePacket(CreateAuthRespFrame(AuthAlgorithm::kOpenSystem)));
  ASSERT_EQ(device.svc_queue.size(), static_cast<size_t>(1));
  auto auths = device.GetServiceMsgs<wlan_mlme::AuthenticateConfirm>(
      fuchsia::wlan::mlme::internal::kMLME_AuthenticateConf_GenOrdinal);
  ASSERT_EQ(auths.size(), 1ULL);
  AssertAuthConfirm(std::move(auths[0]), wlan_mlme::AuthenticateResultCodes::SUCCESS);

  // (sme->mlme) Send ASSOCIATE.request. Verify that no confirmation was sent
  // yet.
  ASSERT_EQ(ZX_OK, client.HandleMlmeMsg(CreateAssocRequest(false)));
  ASSERT_TRUE(device.svc_queue.empty());

  // Verify wlan frame sent to AP is correct.
  ASSERT_EQ(device.wlan_queue.size(), static_cast<size_t>(1));
  AssertAssocReqFrame(std::move(*device.wlan_queue.begin()), false);

  // (ap->mlme) Respond with a Association Response frame and verify a
  // ASSOCIATE.confirm message
  //            was then sent SME.
  ASSERT_EQ(ZX_OK, client.HandleFramePacket(CreateAssocRespFrame()));
  ASSERT_FALSE(device.svc_queue.empty());
  auto assocs = device.GetServiceMsgs<wlan_mlme::AssociateConfirm>(
      fuchsia::wlan::mlme::internal::kMLME_AssociateConf_GenOrdinal);
  ASSERT_EQ(assocs.size(), static_cast<size_t>(1));
  AssertAssocConfirm(std::move(assocs[0]), kAid, wlan_mlme::AssociateResultCodes::SUCCESS);
}

TEST_F(ClientTest, ExchangeEapolFrames) {
  Join();
  Authenticate();
  Associate();

  // (sme->mlme) Send EAPOL.request
  auto&& eapol_req = CreateEapolRequest(common::MacAddr(kClientAddress), common::MacAddr(kBssid1));
  ASSERT_EQ(ZX_OK, client.HandleMlmeMsg(std::move(eapol_req)));

  // Verify EAPOL frame was sent to AP
  ASSERT_EQ(device.wlan_queue.size(), static_cast<size_t>(1));
  auto pkt = std::move(*device.wlan_queue.begin());
  auto frame = TypeCheckWlanFrame<DataFrameView<LlcHeader>>(pkt.pkt.get());
  EXPECT_EQ(std::memcmp(frame.hdr()->addr1.byte, kBssid1, 6), 0);
  EXPECT_EQ(std::memcmp(frame.hdr()->addr2.byte, kClientAddress, 6), 0);
  EXPECT_EQ(std::memcmp(frame.hdr()->addr3.byte, kBssid1, 6), 0);
  EXPECT_EQ(frame.hdr()->fc.protected_frame(), 0);
  EXPECT_EQ(frame.body()->protocol_id_be, htobe16(kEapolProtocolId));
  auto type_checked_frame = frame.SkipHeader().CheckBodyType<EapolHdr>();
  ASSERT_TRUE(type_checked_frame);
  auto llc_eapol_frame = type_checked_frame.CheckLength();
  ASSERT_TRUE(llc_eapol_frame);
  EXPECT_EQ(llc_eapol_frame.body_len(), static_cast<size_t>(5));
  EXPECT_RANGES_EQ(llc_eapol_frame.body_data(), kEapolPdu);
  EXPECT_EQ(pkt.flags, WLAN_TX_INFO_FLAGS_FAVOR_RELIABILITY);
  device.wlan_queue.clear();

  // Verify EAPOL.confirm message was sent to SME
  auto msg_data = device.NextTxMlmeMsg();
  ASSERT_TRUE(msg_data.has_value());
  auto eapol_confirm = MlmeMsg<wlan_mlme::EapolConfirm>::Decode(
      msg_data->data(), fuchsia::wlan::mlme::internal::kMLME_EapolConf_GenOrdinal);
  ASSERT_TRUE(eapol_confirm.has_value());
  EXPECT_EQ(eapol_confirm.value().body()->result_code, wlan_mlme::EapolResultCodes::SUCCESS);

  // After controlled port opens, EAPOL frame has protected flag enabled
  EstablishRsna();
  ASSERT_EQ(ZX_OK, client.HandleMlmeMsg(std::move(CreateEapolRequest(
                       common::MacAddr(kClientAddress), common::MacAddr(kBssid1)))));
  ASSERT_EQ(device.wlan_queue.size(), static_cast<size_t>(1));
  pkt = std::move(*device.wlan_queue.begin());
  frame = TypeCheckWlanFrame<DataFrameView<LlcHeader>>(pkt.pkt.get());
  EXPECT_EQ(frame.hdr()->fc.protected_frame(), 1);
}

TEST_F(ClientTest, SetKeys) {
  Join();
  Authenticate();
  Associate();

  // (sme->mlme) Send SETKEYS.request
  auto key_data = std::vector(std::cbegin(kKeyData), std::cend(kKeyData));
  common::MacAddr bssid(kBssid1);
  client.HandleMlmeMsg(CreateSetKeysRequest(bssid, key_data, wlan_mlme::KeyType::PAIRWISE));

  ASSERT_EQ(device.GetKeys().size(), static_cast<size_t>(1));
  auto key_config = device.GetKeys()[0];
  EXPECT_EQ(std::memcmp(key_config.key, kKeyData, sizeof(kKeyData)), 0);
  EXPECT_EQ(key_config.key_idx, 1);
  EXPECT_EQ(key_config.key_type, WLAN_KEY_TYPE_PAIRWISE);
  EXPECT_EQ(std::memcmp(key_config.peer_addr, bssid.byte, sizeof(bssid)), 0);
  EXPECT_EQ(std::memcmp(key_config.cipher_oui, kCipherOui, sizeof(kCipherOui)), 0);
  EXPECT_EQ(key_config.cipher_type, kCipherSuiteType);
}

TEST_F(ClientTest, ConstructAssociateContext) {
  Join();
  Authenticate();

  // Send ASSOCIATE.request. Verify that no confirmation was sent yet.
  ASSERT_EQ(ZX_OK, client.HandleMlmeMsg(CreateAssocRequest(false)));
  // Respond with a Association Response frame and verify a ASSOCIATE.confirm
  // message was sent.
  auto ap_assoc_ctx = wlan::test_utils::FakeAssocCtx();
  ap_assoc_ctx.vht_cap = {};
  ap_assoc_ctx.vht_op = {};
  ASSERT_EQ(ZX_OK, client.HandleFramePacket(CreateAssocRespFrame(ap_assoc_ctx)));
  auto sta_assoc_ctx = device.GetStationAssocContext();

  ASSERT_TRUE(sta_assoc_ctx != nullptr);
  EXPECT_EQ(sta_assoc_ctx->aid, kAid);
  EXPECT_EQ(sta_assoc_ctx->listen_interval, 0);
  EXPECT_EQ(sta_assoc_ctx->phy, WLAN_INFO_PHY_TYPE_HT);
  EXPECT_EQ(sta_assoc_ctx->chan.primary, 36);
  EXPECT_EQ(sta_assoc_ctx->chan.cbw, WLAN_CHANNEL_BANDWIDTH__40);
  EXPECT_TRUE(sta_assoc_ctx->has_ht_cap);
  EXPECT_TRUE(sta_assoc_ctx->has_ht_op);
  EXPECT_FALSE(sta_assoc_ctx->has_vht_cap);
  EXPECT_FALSE(sta_assoc_ctx->has_vht_op);
}

TEST_F(ClientTest, AuthTimeout) {
  Join();

  // (sme->mlme) Send AUTHENTICATE.request. Verify that no confirmation was sent
  // yet.
  ASSERT_EQ(ZX_OK, client.HandleMlmeMsg(CreateAuthRequest()));
  ASSERT_TRUE(device.svc_queue.empty());

  // Timeout not yet hit.
  SetTimeInBeaconPeriods(kAuthTimeout - 1);
  TriggerTimeout();
  ASSERT_TRUE(device.svc_queue.empty());

  // Timeout hit, verify a AUTHENTICATION.confirm message was sent to SME.
  SetTimeInBeaconPeriods(kAuthTimeout);
  TriggerTimeout();
  ASSERT_EQ(device.svc_queue.size(), static_cast<size_t>(1));
  auto auths = device.GetServiceMsgs<wlan_mlme::AuthenticateConfirm>(
      fuchsia::wlan::mlme::internal::kMLME_AuthenticateConf_GenOrdinal);
  ASSERT_EQ(auths.size(), 1ULL);
  AssertAuthConfirm(std::move(auths[0]), wlan_mlme::AuthenticateResultCodes::AUTH_FAILURE_TIMEOUT);
}

TEST_F(ClientTest, AssocTimeout) {
  Join();
  Authenticate();

  // (sme->mlme) Send ASSOCIATE.request. Verify that no confirmation was sent
  // yet.
  ASSERT_EQ(ZX_OK, client.HandleMlmeMsg(CreateAssocRequest(false)));
  ASSERT_TRUE(device.svc_queue.empty());

  // Timeout not yet hit.
  SetTimeInBeaconPeriods(10);
  TriggerTimeout();
  ASSERT_TRUE(device.svc_queue.empty());

  // Timeout hit, verify a ASSOCIATE.confirm message was sent to SME.
  SetTimeInBeaconPeriods(40);
  TriggerTimeout();
  ASSERT_EQ(device.svc_queue.size(), static_cast<size_t>(1));
  auto assocs = device.GetServiceMsgs<wlan_mlme::AssociateConfirm>(
      fuchsia::wlan::mlme::internal::kMLME_AssociateConf_GenOrdinal);
  ASSERT_EQ(assocs.size(), 1ULL);
  AssertAssocConfirm(std::move(assocs[0]), 0, wlan_mlme::AssociateResultCodes::REFUSED_TEMPORARILY);
}

TEST_F(ClientTest, ReceiveDataAfterAssociation_Protected) {
  // Verify no data frame can be received before RSNA is established.
  Join();
  client.HandleFramePacket(CreateDataFrame(kTestPayload));
  ASSERT_TRUE(device.AreQueuesEmpty());

  Authenticate();
  client.HandleFramePacket(CreateDataFrame(kTestPayload));
  ASSERT_TRUE(device.AreQueuesEmpty());

  Associate();
  client.HandleFramePacket(CreateDataFrame(kTestPayload));
  ASSERT_TRUE(device.AreQueuesEmpty());

  // Setting key does not open controlled port
  SetKey();
  client.HandleFramePacket(CreateDataFrame(kTestPayload));
  ASSERT_TRUE(device.AreQueuesEmpty());

  // Establish RSNA and verify data frame can be received
  EstablishRsna();
  client.HandleFramePacket(CreateDataFrame(kTestPayload));
  auto eth_frames = device.GetEthPackets();
  ASSERT_EQ(eth_frames.size(), static_cast<size_t>(1));
  ASSERT_TRUE(device.wlan_queue.empty());
  ASSERT_TRUE(device.svc_queue.empty());
}

TEST_F(ClientTest, SendDataAfterAssociation_Protected) {
  // Verify no data frame can be sent before association
  Join();
  client.HandleFramePacket(CreateEthFrame(kTestPayload));
  ASSERT_TRUE(device.AreQueuesEmpty());

  Authenticate();
  client.HandleFramePacket(CreateEthFrame(kTestPayload));
  ASSERT_TRUE(device.AreQueuesEmpty());

  // After association but before RSNA is established, data frame is sent out
  // but unprotected
  Associate();
  client.HandleFramePacket(CreateEthFrame(kTestPayload));
  EXPECT_EQ(device.wlan_queue.size(), static_cast<size_t>(1));
  AssertDataFrameSentToAp(std::move(*device.wlan_queue.begin()), kTestPayload);
  device.wlan_queue.clear();

  // Setting key does not open controlled port, so data frame is still
  // unprotected
  SetKey();
  client.HandleFramePacket(CreateEthFrame(kTestPayload));
  EXPECT_EQ(device.wlan_queue.size(), static_cast<size_t>(1));
  AssertDataFrameSentToAp(std::move(*device.wlan_queue.begin()), kTestPayload);
  device.wlan_queue.clear();

  // After RSNA is established, outbound data frames have `protected_frame` flag
  // enabled
  EstablishRsna();
  client.HandleFramePacket(CreateEthFrame(kTestPayload));
  EXPECT_EQ(device.wlan_queue.size(), static_cast<size_t>(1));
  AssertDataFrameSentToAp(std::move(*device.wlan_queue.begin()), kTestPayload,
                          {.protected_frame = 1});
}

TEST_F(ClientTest, SendKeepAliveFrameAfterAssociation_Protected) {
  // Verify client doesn't respond to null data frame before association.
  Join();
  SendNullDataFrame();
  ASSERT_TRUE(device.AreQueuesEmpty());

  Authenticate();
  SendNullDataFrame();
  ASSERT_TRUE(device.AreQueuesEmpty());

  // After association, when client receives null data frame, "Keep Alive"
  // response is sent out
  Associate();
  SendNullDataFrame();
  ASSERT_EQ(device.wlan_queue.size(), static_cast<size_t>(1));
  ASSERT_TRUE(device.svc_queue.empty());
  AssertKeepAliveFrame(std::move(*device.wlan_queue.begin()));
  device.wlan_queue.clear();

  EstablishRsna();
  SendNullDataFrame();
  ASSERT_EQ(device.wlan_queue.size(), static_cast<size_t>(1));
  ASSERT_TRUE(device.svc_queue.empty());
  AssertKeepAliveFrame(std::move(*device.wlan_queue.begin()));
}

TEST_F(ClientTest, ReceiveDataAfterAssociation_Unprotected) {
  // Verify no data frame can be received before association.
  Join(false);
  client.HandleFramePacket(CreateDataFrame(kTestPayload));
  ASSERT_TRUE(device.AreQueuesEmpty());

  Authenticate();
  client.HandleFramePacket(CreateDataFrame(kTestPayload));
  ASSERT_TRUE(device.AreQueuesEmpty());

  // Associate and verify data frame can be received.
  Associate(false);
  client.HandleFramePacket(CreateDataFrame(kTestPayload));
  auto eth_frames = device.GetEthPackets();
  ASSERT_EQ(eth_frames.size(), static_cast<size_t>(1));
  ASSERT_TRUE(device.wlan_queue.empty());
  ASSERT_TRUE(device.svc_queue.empty());
}

TEST_F(ClientTest, SendDataAfterAssociation_Unprotected) {
  // Verify no data frame can be sent before association.
  Join(false);
  client.HandleFramePacket(CreateEthFrame(kTestPayload));
  ASSERT_TRUE(device.AreQueuesEmpty());

  Authenticate();
  client.HandleFramePacket(CreateEthFrame(kTestPayload));
  ASSERT_TRUE(device.AreQueuesEmpty());

  // Associate and verify that data frame can be sent out.
  Associate(false);
  client.HandleFramePacket(CreateEthFrame(kTestPayload));
  EXPECT_EQ(device.wlan_queue.size(), static_cast<size_t>(1));
  AssertDataFrameSentToAp(std::move(*device.wlan_queue.begin()), kTestPayload);
}

TEST_F(ClientTest, SendKeepAliveFrameAfterAssociation_Unprotected) {
  // Verify client doesn't respond to null data frame before association.
  Join(false);
  SendNullDataFrame();
  ASSERT_TRUE(device.AreQueuesEmpty());

  Authenticate();
  SendNullDataFrame();
  ASSERT_TRUE(device.AreQueuesEmpty());

  // After association, when client receives null data frame, "Keep Alive"
  // response is sent out
  Associate(false);
  SendNullDataFrame();
  ASSERT_EQ(device.wlan_queue.size(), static_cast<size_t>(1));
  ASSERT_TRUE(device.svc_queue.empty());
  AssertKeepAliveFrame(std::move(*device.wlan_queue.begin()));
}

TEST_F(ClientTest, ProcessEmptyDataFrames) {
  Connect();

  // Send a data frame which carries an LLC frame with no payload.
  // Verify no ethernet frame was queued.
  client.HandleFramePacket(CreateDataFrame({}));
  ASSERT_TRUE(device.eth_queue.empty());
}

TEST_F(ClientTest, ProcessAmsduDataFrame) {
  const uint8_t payload_data[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  fbl::Span<const uint8_t> payload(payload_data);
  std::vector<fbl::Span<const uint8_t>> payloads;
  for (size_t payload_len = 1; payload_len <= 10; ++payload_len) {
    payloads.push_back(payload.subspan(0, payload_len));
  }

  Connect();
  client.HandleFramePacket(CreateAmsduDataFramePacket(payloads));
  ASSERT_EQ(device.eth_queue.size(), payloads.size());
  for (size_t i = 0; i < payloads.size(); ++i) {
    auto eth_payload = fbl::Span<const uint8_t>(device.eth_queue[i]).subspan(sizeof(EthernetII));
    EXPECT_RANGES_EQ(eth_payload, payloads[i]);
  }
}

TEST_F(ClientTest, DropManagementFrames) {
  Connect();

  // Construct and send deauthentication frame from another BSS.
  constexpr size_t max_frame_len = MgmtFrameHeader::max_len() + Deauthentication::max_len();
  auto packet = GetWlanPacket(max_frame_len);
  ASSERT_NE(packet, nullptr);

  BufferWriter w(*packet);
  auto mgmt_hdr = w.Write<MgmtFrameHeader>();
  mgmt_hdr->fc.set_type(FrameType::kManagement);
  mgmt_hdr->fc.set_subtype(ManagementSubtype::kDeauthentication);
  mgmt_hdr->addr1 = common::MacAddr(kBssid2);
  mgmt_hdr->addr2 = common::MacAddr(kClientAddress);
  mgmt_hdr->addr3 = common::MacAddr(kBssid2);
  w.Write<Deauthentication>()->reason_code = 42;
  client.HandleFramePacket(std::move(packet));

  // Verify neither a management frame nor service message were sent.
  ASSERT_TRUE(device.svc_queue.empty());
  ASSERT_TRUE(device.wlan_queue.empty());
  ASSERT_TRUE(device.eth_queue.empty());

  // Verify data frames can still be send and the clientis presumably
  // associated.
  client.HandleFramePacket(CreateDataFrame(kTestPayload));
  ASSERT_EQ(device.eth_queue.size(), static_cast<size_t>(1));
}

TEST_F(ClientTest, AutoDeauth_NoBeaconReceived) {
  Connect();

  // Timeout not yet hit.
  IncreaseTimeByBeaconPeriods(kAutoDeauthTimeout - 1);
  TriggerTimeout();
  ASSERT_TRUE(device.wlan_queue.empty());
  auto deauth_inds = device.GetServiceMsgs<wlan_mlme::DeauthenticateIndication>(
      fuchsia::wlan::mlme::internal::kMLME_DeauthenticateInd_GenOrdinal);
  ASSERT_EQ(deauth_inds.size(), 0ULL);

  // Auto-deauth timeout, client should be deauthenticated.
  IncreaseTimeByBeaconPeriods(1);
  TriggerTimeout();
  ASSERT_EQ(device.wlan_queue.size(), static_cast<size_t>(1));
  AssertDeauthFrame(std::move(*device.wlan_queue.begin()),
                    wlan_mlme::ReasonCode::LEAVING_NETWORK_DEAUTH);
  deauth_inds = device.GetServiceMsgs<wlan_mlme::DeauthenticateIndication>(
      fuchsia::wlan::mlme::internal::kMLME_DeauthenticateInd_GenOrdinal);
  ASSERT_EQ(deauth_inds.size(), 1ULL);
}

TEST_F(ClientTest, AutoDeauth_NoBeaconsShortlyAfterConnecting) {
  Connect();

  IncreaseTimeByBeaconPeriods(1);
  SendBeaconFrame();

  // Not enough time has passed yet since beacon frame was sent, so no deauth.
  IncreaseTimeByBeaconPeriods(kAutoDeauthTimeout - 1);
  TriggerTimeout();
  ASSERT_TRUE(device.wlan_queue.empty());

  // Auto-deauth triggers now.
  IncreaseTimeByBeaconPeriods(1);
  TriggerTimeout();
  ASSERT_EQ(device.wlan_queue.size(), static_cast<size_t>(1));
  AssertDeauthFrame(std::move(*device.wlan_queue.begin()),
                    wlan_mlme::ReasonCode::LEAVING_NETWORK_DEAUTH);
  auto deauth_inds = device.GetServiceMsgs<wlan_mlme::DeauthenticateIndication>(
      fuchsia::wlan::mlme::internal::kMLME_DeauthenticateInd_GenOrdinal);
  ASSERT_EQ(deauth_inds.size(), 1ULL);
}

// Generally comment of auto-deauth tests below that combine with switching channel:
// - These tests make sure that the combined on-channel times without receiving beacon
//   is a full auto-deauth interval.
// - How long the off-channel times are do not matter, but we set them long enough to
//   be confident that off-channel times do not contribute to auto-deauth.
TEST_F(ClientTest, AutoDeauth_DoNotDeauthWhileSwitchingChannel) {
  Connect();

  IncreaseTimeByBeaconPeriods(kAutoDeauthTimeout - 1);
  // Off channel time is arbitrary, but should match the total time we advance before
  // the `TriggerTimeoutToGoOnChannel` call.
  GoOffChannel(2 * kAutoDeauthTimeout + 1);

  // For next two timeouts, still off channel, so should not deauth.
  IncreaseTimeByBeaconPeriods(1);
  TriggerTimeout();
  ASSERT_FALSE(client.OnChannel());
  ASSERT_TRUE(device.wlan_queue.empty());

  IncreaseTimeByBeaconPeriods(kAutoDeauthTimeout);
  TriggerTimeout();
  ASSERT_FALSE(client.OnChannel());
  ASSERT_TRUE(device.wlan_queue.empty());

  // Have not been back on main channel for long enough, so should not deauth.
  IncreaseTimeByBeaconPeriods(kAutoDeauthTimeout);
  TriggerTimeoutToGoOnChannel();

  // Before going off channel, we did not receive beacon for `kAutoDeauthTimeout
  // - 1` period. Now one more beacon period has passed after going back on
  // channel, so should auto deauth.
  IncreaseTimeByBeaconPeriods(1);
  TriggerTimeout();
  ASSERT_EQ(device.wlan_queue.size(), static_cast<size_t>(1));
  AssertDeauthFrame(std::move(*device.wlan_queue.begin()),
                    wlan_mlme::ReasonCode::LEAVING_NETWORK_DEAUTH);
  auto deauth_inds = device.GetServiceMsgs<wlan_mlme::DeauthenticateIndication>(
      fuchsia::wlan::mlme::internal::kMLME_DeauthenticateInd_GenOrdinal);
  ASSERT_EQ(deauth_inds.size(), 1ULL);
}

TEST_F(ClientTest, AutoDeauth_InterleavingBeaconsAndChannelSwitches) {
  Connect();

  // Going off channel.
  IncreaseTimeByBeaconPeriods(kAutoDeauthTimeout - 5);  // -- On-channel time without beacon -- //
  // Off channel time is arbitrary, but should match the total time we advance before
  // the `TriggerTimeoutToGoOnChannel` call.
  GoOffChannel(6);

  // No deauth since off channel.
  IncreaseTimeByBeaconPeriods(5);
  TriggerTimeout();
  ASSERT_FALSE(client.OnChannel());
  ASSERT_TRUE(device.wlan_queue.empty());

  IncreaseTimeByBeaconPeriods(1);
  TriggerTimeoutToGoOnChannel();

  // Got beacon frame, which should reset the timeout.
  IncreaseTimeByBeaconPeriods(3);  // -- On-channel time without beacon  -- //
  SendBeaconFrame();               // -- Beacon timeout refresh -- ///

  // No deauth since beacon was received not too long ago.
  IncreaseTimeByBeaconPeriods(2);  // -- On-channel time without beacon  -- //
  TriggerTimeout();
  ASSERT_TRUE(device.wlan_queue.empty());

  // Going off channel and back on channel
  // Total on-channel time without beacons so far: 2 beacon intervals
  GoOffChannel(kAutoDeauthTimeout);
  IncreaseTimeByBeaconPeriods(kAutoDeauthTimeout);
  TriggerTimeoutToGoOnChannel();

  IncreaseTimeByBeaconPeriods(kAutoDeauthTimeout - 3);  // -- On-channel time without beacon -- //
  TriggerTimeout();
  ASSERT_TRUE(device.wlan_queue.empty());

  // Going off channel and back on channel again
  // Total on-channel time without beacons so far: 2 + kAutoDeauthTimeout - 3
  GoOffChannel(kAutoDeauthTimeout);
  IncreaseTimeByBeaconPeriods(kAutoDeauthTimeout);
  TriggerTimeoutToGoOnChannel();
  ASSERT_TRUE(device.wlan_queue.empty());

  // One more beacon period and auto-deauth triggers
  IncreaseTimeByBeaconPeriods(1);  // -- On-channel time without beacon -- //
  TriggerTimeout();
  ASSERT_EQ(device.wlan_queue.size(), static_cast<size_t>(1));
  AssertDeauthFrame(std::move(*device.wlan_queue.begin()),
                    wlan_mlme::ReasonCode::LEAVING_NETWORK_DEAUTH);
  auto deauth_inds = device.GetServiceMsgs<wlan_mlme::DeauthenticateIndication>(
      fuchsia::wlan::mlme::internal::kMLME_DeauthenticateInd_GenOrdinal);
  ASSERT_EQ(deauth_inds.size(), 1ULL);
}

// This test explores what happens if the whole auto-deauth timeout duration is
// exhausted, but the client switches channel before auto-deauth can trigger.
// For the current implementation where we cancel timer when going off channel
// and reschedule when going back on channel, this test is intended to be a
// safeguard against making the mistake of scheduling or exactly in the present
// when going back on channel.
TEST_F(ClientTest, AutoDeauth_SwitchingChannelBeforeDeauthTimeoutCouldTrigger) {
  Connect();

  // No deauth since off channel.
  IncreaseTimeByBeaconPeriods(kAutoDeauthTimeout);
  // Off channel time is arbitrary, but should match the total time we advance before
  // the `TriggerTimeoutToGoOnChannel` call.
  GoOffChannel(1);
  TriggerTimeout();
  ASSERT_FALSE(client.OnChannel());
  auto deauth_inds = device.GetServiceMsgs<wlan_mlme::DeauthenticateIndication>(
      fuchsia::wlan::mlme::internal::kMLME_DeauthenticateInd_GenOrdinal);
  ASSERT_EQ(deauth_inds.size(), 0ULL);
  ASSERT_TRUE(device.wlan_queue.empty());

  IncreaseTimeByBeaconPeriods(1);
  TriggerTimeoutToGoOnChannel();

  // Auto-deauth timeout shouldn't trigger yet. This is because after going back
  // on channel, the client should always schedule timeout sufficiently far
  // enough in the future (at least one beacon interval)
  TriggerTimeout();
  ASSERT_TRUE(device.wlan_queue.empty());

  // Auto-deauth now
  IncreaseTimeByBeaconPeriods(1);
  TriggerTimeout();
  ASSERT_EQ(device.wlan_queue.size(), static_cast<size_t>(1));
  AssertDeauthFrame(std::move(*device.wlan_queue.begin()),
                    wlan_mlme::ReasonCode::LEAVING_NETWORK_DEAUTH);
  deauth_inds = device.GetServiceMsgs<wlan_mlme::DeauthenticateIndication>(
      fuchsia::wlan::mlme::internal::kMLME_DeauthenticateInd_GenOrdinal);
  ASSERT_EQ(deauth_inds.size(), 1ULL);
}

TEST_F(ClientTest, AutoDeauth_ForeignBeaconShouldNotPreventDeauth) {
  Connect();

  IncreaseTimeByBeaconPeriods(kAutoDeauthTimeout - 1);
  SendBeaconFrame(common::MacAddr(kBssid2));  // beacon frame from another AP

  IncreaseTimeByBeaconPeriods(1);
  TriggerTimeout();
  ASSERT_EQ(device.wlan_queue.size(), static_cast<size_t>(1));
  AssertDeauthFrame(std::move(*device.wlan_queue.begin()),
                    wlan_mlme::ReasonCode::LEAVING_NETWORK_DEAUTH);
  auto deauth_inds = device.GetServiceMsgs<wlan_mlme::DeauthenticateIndication>(
      fuchsia::wlan::mlme::internal::kMLME_DeauthenticateInd_GenOrdinal);
  ASSERT_EQ(deauth_inds.size(), 1ULL);
}

TEST_F(ClientTest, DropFramesWhileOffChannel) {
  Connect();

  GoOffChannel(1);
  client.HandleFramePacket(CreateEthFrame(kTestPayload));
  ASSERT_TRUE(device.wlan_queue.empty());

  IncreaseTimeByBeaconPeriods(1);
  TriggerTimeoutToGoOnChannel();
  ASSERT_EQ(device.wlan_queue.size(), static_cast<size_t>(0));
}

TEST_F(ClientTest, InvalidAuthenticationResponse) {
  Join();

  // Send AUTHENTICATION.request. Verify that no confirmation was sent yet.
  ASSERT_EQ(ZX_OK, client.HandleMlmeMsg(CreateAuthRequest()));
  ASSERT_TRUE(device.svc_queue.empty());

  // Send authentication frame with wrong algorithm.
  ASSERT_EQ(ZX_OK, client.HandleFramePacket(CreateAuthRespFrame(AuthAlgorithm::kSae)));

  // Verify that AUTHENTICATION.confirm was received.
  ASSERT_EQ(device.svc_queue.size(), static_cast<size_t>(1));
  auto auths = device.GetServiceMsgs<wlan_mlme::AuthenticateConfirm>(
      fuchsia::wlan::mlme::internal::kMLME_AuthenticateConf_GenOrdinal);
  ASSERT_EQ(auths.size(), 1ULL);
  AssertAuthConfirm(std::move(auths[0]),
                    wlan_mlme::AuthenticateResultCodes::AUTHENTICATION_REJECTED);

  // Fast forward in time would have caused a timeout.
  // The timeout however should have been canceled and we should not receive
  // and additional confirmation.
  SetTimeInBeaconPeriods(kAuthTimeout);
  TriggerTimeout();
  ASSERT_TRUE(device.svc_queue.empty());

  // Send a second, now valid authentication frame.
  // This frame should be ignored as the client reset.
  ASSERT_EQ(ZX_OK, client.HandleFramePacket(CreateAuthRespFrame(AuthAlgorithm::kOpenSystem)));

  // Fast forward in time far beyond an authentication timeout.
  // There should not be any AUTHENTICATION.confirm sent as the client
  // is expected to have been reset into |idle| state after failing
  // to authenticate.
  SetTimeInBeaconPeriods(1000);
  TriggerTimeout();
  ASSERT_TRUE(device.svc_queue.empty());
}

TEST_F(ClientTest, ProcessZeroRssiFrame) {
  auto no_rssi_pkt = CreateDataFrame(kTestPayload);
  auto rx_info = const_cast<wlan_rx_info_t*>(no_rssi_pkt->ctrl_data<wlan_rx_info_t>());
  rx_info->valid_fields &= ~WLAN_RX_INFO_VALID_DATA_RATE;  // no rssi
  rx_info->rssi_dbm = 0;

  auto rssi_pkt = CreateDataFrame(kTestPayload);
  rx_info = const_cast<wlan_rx_info_t*>(rssi_pkt->ctrl_data<wlan_rx_info_t>());
  rx_info->valid_fields |= WLAN_RX_INFO_VALID_DATA_RATE;
  rx_info->rssi_dbm = 0;

  Connect();

  ASSERT_GT(client.GetMlmeStats().client_mlme_stats().assoc_data_rssi.hist.size(), 0u);
  ASSERT_EQ(client.GetMlmeStats().client_mlme_stats().assoc_data_rssi.hist[0], 0u);

  // Send a data frame with no rssi and verify that we don't increment stats.
  ASSERT_EQ(ZX_OK, client.HandleFramePacket(std::move(no_rssi_pkt)));
  ASSERT_EQ(client.GetMlmeStats().client_mlme_stats().assoc_data_rssi.hist[0], 0u);

  // Send a data frame with 0 rssi and verify that we *do* increment stats.
  ASSERT_EQ(ZX_OK, client.HandleFramePacket(std::move(rssi_pkt)));
  ASSERT_EQ(client.GetMlmeStats().client_mlme_stats().assoc_data_rssi.hist[0], 1u);
}

TEST_F(ClientTest, PsPollWithMoreData) {
  Connect();

  auto more_data_pkt = CreateDataFrame(kTestPayload);
  more_data_pkt->mut_field<DataFrameHeader>(0)->fc.set_more_data(true);
  more_data_pkt->mut_field<DataFrameHeader>(0)->addr1 = common::MacAddr(kClientAddress);

  ASSERT_EQ(ZX_OK, client.HandleFramePacket(std::move(more_data_pkt)));

  ASSERT_EQ(device.wlan_queue.size(), 1ULL);
  auto frame = TypeCheckWlanFrame<CtrlFrameView<PsPollFrame>>(device.wlan_queue[0].pkt.get());
  ASSERT_TRUE(frame);

  EXPECT_EQ(ControlSubtype::kPsPoll, frame.hdr()->fc.subtype());
  EXPECT_EQ(kAid | 0xc000, frame.body()->aid);
  EXPECT_EQ(common::MacAddr(kBssid1), frame.body()->bssid);
  EXPECT_EQ(common::MacAddr(kClientAddress), frame.body()->ta);
}

TEST_F(ClientTest, PsPollWithBeacon) {
  Connect();

  constexpr size_t reserved_ie_len = 256;
  constexpr size_t max_frame_len =
      MgmtFrameHeader::max_len() + ProbeRequest::max_len() + reserved_ie_len;
  auto beacon_pkt = GetWlanPacket(max_frame_len);

  BufferWriter w(*beacon_pkt);
  w.Write(*CreateBeaconFrame(common::MacAddr(kBssid1)));

  TrafficIndicationMap tim;
  tim.SetTrafficIndication(kAid, true);

  size_t bitmap_len;
  uint8_t bitmap_offset;
  uint8_t pvb[kMaxTimBitmapLen];
  EXPECT_EQ(ZX_OK, tim.WritePartialVirtualBitmap(pvb, sizeof(pvb), &bitmap_len, &bitmap_offset));

  TimHeader header;
  header.dtim_count = 0;
  header.dtim_period = 0;
  header.bmp_ctrl.set_offset(bitmap_offset);

  common::WriteTim(&w, header, {pvb, bitmap_len});

  beacon_pkt->set_len(w.WrittenBytes());

  // rx_info is out of band, so we have to copy it back into beacon_pkt here.
  wlan_rx_info_t rx_info{.rx_flags = 0};
  beacon_pkt->CopyCtrlFrom(rx_info);

  client.HandleFramePacket(std::move(beacon_pkt));

  ASSERT_EQ(device.wlan_queue.size(), 1ULL);
  auto frame = TypeCheckWlanFrame<CtrlFrameView<PsPollFrame>>(device.wlan_queue[0].pkt.get());
  ASSERT_TRUE(frame);

  EXPECT_EQ(ControlSubtype::kPsPoll, frame.hdr()->fc.subtype());
  EXPECT_EQ(kAid | 0xc000, frame.body()->aid);
  EXPECT_EQ(common::MacAddr(kBssid1), frame.body()->bssid);
  EXPECT_EQ(common::MacAddr(kClientAddress), frame.body()->ta);
}

// Add additional tests for (tracked in NET-801):
// AP refuses Authentication/Association
// Regression tests for:
// - NET-898: PS-POLL after TIM indication.
// Deauthenticate in any state issued by AP/SME.
// Disassociation in any state issued by AP/SME.
// Handle Action frames and setup Block-Ack session.
// Drop data frames from unknown BSS.
// Connect to a:
// - HT/VHT capable network
// - 5GHz network
// - different network than currently associated to
// Notify driver about association
// Ensure Deauthentication Indicaiton and notification is sent whenever
// deauthenticating. Enter/Leave power management when going off/on channel.
// Verify timeouts don't hit after resetting the station.

}  // namespace
}  // namespace wlan
