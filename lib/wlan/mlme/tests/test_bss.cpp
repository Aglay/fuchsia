// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "test_bss.h"
#include "mock_device.h"

#include <wlan/common/buffer_writer.h>
#include <wlan/common/channel.h>
#include <wlan/common/write_element.h>
#include <wlan/mlme/ap/bss_interface.h>
#include <wlan/mlme/debug.h>
#include <wlan/mlme/mac_frame.h>
#include <wlan/mlme/packet.h>
#include <wlan/mlme/rates_elements.h>
#include <wlan/mlme/service.h>

#include <fbl/unique_ptr.h>
#include <fuchsia/wlan/mlme/c/fidl.h>
#include <fuchsia/wlan/mlme/cpp/fidl.h>

#include <gtest/gtest.h>

namespace wlan {

namespace wlan_mlme = ::fuchsia::wlan::mlme;

void WriteTim(BufferWriter* w, const PsCfg& ps_cfg) {
    size_t bitmap_len = ps_cfg.GetTim()->BitmapLen();
    uint8_t bitmap_offset = ps_cfg.GetTim()->BitmapOffset();

    TimHeader hdr;
    hdr.dtim_count = ps_cfg.dtim_count();
    hdr.dtim_period = ps_cfg.dtim_period();
    ZX_DEBUG_ASSERT(hdr.dtim_count != hdr.dtim_period);
    if (hdr.dtim_count == hdr.dtim_period) { warnf("illegal DTIM state"); }

    hdr.bmp_ctrl.set_offset(bitmap_offset);
    if (ps_cfg.IsDtim()) { hdr.bmp_ctrl.set_group_traffic_ind(ps_cfg.GetTim()->HasGroupTraffic()); }
    common::WriteTim(w, hdr, {ps_cfg.GetTim()->BitmapData(), bitmap_len});
}

void WriteCountry(BufferWriter* w, const wlan_channel_t chan) {
    const Country kCountry = {{'U', 'S', ' '}};

    std::vector<SubbandTriplet> subbands;

    // TODO(porce): Read from the AP's regulatory domain
    if (wlan::common::Is2Ghz(chan)) {
        subbands.push_back({1, 11, 36});
    } else {
        subbands.push_back({36, 4, 36});
        subbands.push_back({52, 4, 30});
        subbands.push_back({100, 12, 30});
        subbands.push_back({149, 5, 36});
    }

    common::WriteCountry(w, kCountry, subbands);
}

wlan_mlme::BSSDescription CreateBssDescription() {
    common::MacAddr bssid(kBssid1);

    wlan_mlme::BSSDescription bss_desc;
    std::memcpy(bss_desc.bssid.mutable_data(), bssid.byte, common::kMacAddrLen);
    std::vector<uint8_t> ssid(kSsid, kSsid + sizeof(kSsid));
    bss_desc.ssid.reset(std::move(ssid));
    bss_desc.bss_type = wlan_mlme::BSSTypes::INFRASTRUCTURE;
    bss_desc.beacon_period = kBeaconPeriodTu;
    bss_desc.dtim_period = kDtimPeriodTu;
    bss_desc.timestamp = 0;
    bss_desc.local_time = 0;
    bss_desc.basic_rate_set.resize(0);
    bss_desc.op_rate_set.resize(0);

    wlan_mlme::CapabilityInfo cap;
    cap.ess = true;
    cap.short_preamble = true;
    bss_desc.cap = cap;

    bss_desc.rsn.reset();
    bss_desc.rcpi_dbmh = 0;
    bss_desc.rsni_dbh = 0;

    bss_desc.ht_cap.reset();
    bss_desc.ht_op.reset();

    bss_desc.vht_cap.reset();
    bss_desc.vht_op.reset();

    bss_desc.chan.cbw = static_cast<wlan_mlme::CBW>(kBssChannel.cbw);
    bss_desc.chan.primary = kBssChannel.primary;

    bss_desc.rssi_dbm = -35;

    return fbl::move(bss_desc);
}

zx_status_t CreateStartRequest(MlmeMsg<wlan_mlme::StartRequest>* out_msg, bool protected_ap) {
    auto req = wlan_mlme::StartRequest::New();
    std::vector<uint8_t> ssid(kSsid, kSsid + sizeof(kSsid));
    req->ssid.reset(std::move(ssid));
    req->bss_type = wlan_mlme::BSSTypes::INFRASTRUCTURE;
    req->beacon_period = kBeaconPeriodTu;
    req->dtim_period = kDtimPeriodTu;
    req->channel = kBssChannel.primary;
    req->mesh_id.resize(0);
    if (protected_ap) { req->rsne.reset(std::vector<uint8_t>(kRsne, kRsne + sizeof(kRsne))); }

    return WriteServiceMessage(req.get(), fuchsia_wlan_mlme_MLMEStartReqOrdinal, out_msg);
}

zx_status_t CreateJoinRequest(MlmeMsg<wlan_mlme::JoinRequest>* out_msg) {
    auto req = wlan_mlme::JoinRequest::New();
    req->join_failure_timeout = kJoinTimeout;
    req->nav_sync_delay = 20;
    req->op_rate_set.reset({10, 22, 34});
    req->selected_bss = CreateBssDescription();

    return WriteServiceMessage(req.get(), fuchsia_wlan_mlme_MLMEJoinReqOrdinal, out_msg);
}

zx_status_t CreateAuthRequest(MlmeMsg<wlan_mlme::AuthenticateRequest>* out_msg) {
    common::MacAddr bssid(kBssid1);

    auto req = wlan_mlme::AuthenticateRequest::New();
    std::memcpy(req->peer_sta_address.mutable_data(), bssid.byte, common::kMacAddrLen);
    req->auth_failure_timeout = kAuthTimeout;
    req->auth_type = wlan_mlme::AuthenticationTypes::OPEN_SYSTEM;

    return WriteServiceMessage(req.get(), fuchsia_wlan_mlme_MLMEAuthenticateReqOrdinal, out_msg);
}

zx_status_t CreateAuthResponse(MlmeMsg<wlan_mlme::AuthenticateResponse>* out_msg,
                               wlan_mlme::AuthenticateResultCodes result_code) {
    common::MacAddr client(kClientAddress);

    auto resp = wlan_mlme::AuthenticateResponse::New();
    std::memcpy(resp->peer_sta_address.mutable_data(), client.byte, common::kMacAddrLen);
    resp->result_code = result_code;

    return WriteServiceMessage(resp.get(), fuchsia_wlan_mlme_MLMEAuthenticateRespOrdinal, out_msg);
}

zx_status_t CreateAssocRequest(MlmeMsg<wlan_mlme::AssociateRequest>* out_msg) {
    common::MacAddr bssid(kBssid1);

    auto req = wlan_mlme::AssociateRequest::New();
    std::memcpy(req->peer_sta_address.mutable_data(), bssid.byte, common::kMacAddrLen);
    req->rsn.reset();

    return WriteServiceMessage(req.get(), fuchsia_wlan_mlme_MLMEAssociateReqOrdinal, out_msg);
}

zx_status_t CreateAssocResponse(MlmeMsg<wlan_mlme::AssociateResponse>* out_msg,
                                wlan_mlme::AssociateResultCodes result_code) {
    common::MacAddr client(kClientAddress);

    auto resp = wlan_mlme::AssociateResponse::New();
    std::memcpy(resp->peer_sta_address.mutable_data(), client.byte, common::kMacAddrLen);
    resp->result_code = result_code;
    resp->association_id = kAid;

    return WriteServiceMessage(resp.get(), fuchsia_wlan_mlme_MLMEAssociateRespOrdinal, out_msg);
}

zx_status_t CreateEapolRequest(MlmeMsg<wlan_mlme::EapolRequest>* out_msg) {
    common::MacAddr bssid(kBssid1);
    common::MacAddr client(kClientAddress);

    auto req = wlan_mlme::EapolRequest::New();
    std::memcpy(req->dst_addr.mutable_data(), client.byte, common::kMacAddrLen);
    std::memcpy(req->src_addr.mutable_data(), bssid.byte, common::kMacAddrLen);
    std::vector<uint8_t> eapol_pdu(kEapolPdu, kEapolPdu + sizeof(kEapolPdu));
    req->data.reset(std::move(eapol_pdu));

    return WriteServiceMessage(req.get(), fuchsia_wlan_mlme_MLMEEapolReqOrdinal, out_msg);
}

zx_status_t CreateSetKeysRequest(MlmeMsg<wlan_mlme::SetKeysRequest>* out_msg,
                                 std::vector<uint8_t> key_data, wlan_mlme::KeyType key_type) {
    wlan_mlme::SetKeyDescriptor key;
    key.key.reset(key_data);
    key.key_id = 1;
    key.key_type = key_type;
    std::memcpy(key.address.mutable_data(), kClientAddress, sizeof(kClientAddress));
    std::memcpy(key.cipher_suite_oui.mutable_data(), kCipherOui, sizeof(kCipherOui));
    key.cipher_suite_type = kCipherSuiteType;

    std::vector<wlan_mlme::SetKeyDescriptor> keylist;
    keylist.emplace_back(std::move(key));
    auto req = wlan_mlme::SetKeysRequest::New();
    req->keylist.reset(std::move(keylist));

    return WriteServiceMessage(req.get(), fuchsia_wlan_mlme_MLMESetKeysReqOrdinal, out_msg);
}

zx_status_t CreateBeaconFrame(fbl::unique_ptr<Packet>* out_packet) {
    return CreateBeaconFrameWithBssid(out_packet, common::MacAddr(kBssid1));
}

zx_status_t CreateBeaconFrameWithBssid(fbl::unique_ptr<Packet>* out_packet, common::MacAddr bssid) {
    constexpr size_t ie_len = 256;
    constexpr size_t max_frame_len = MgmtFrameHeader::max_len() + Beacon::max_len() + ie_len;
    auto packet = GetWlanPacket(max_frame_len);
    if (packet == nullptr) { return ZX_ERR_NO_RESOURCES; }

    BufferWriter w(*packet);
    auto mgmt_hdr = w.Write<MgmtFrameHeader>();
    mgmt_hdr->fc.set_type(FrameType::kManagement);
    mgmt_hdr->fc.set_subtype(ManagementSubtype::kBeacon);
    mgmt_hdr->addr1 = common::kBcastMac;
    mgmt_hdr->addr2 = bssid;
    mgmt_hdr->addr3 = bssid;

    auto bcn = w.Write<Beacon>();
    bcn->beacon_interval = kBeaconPeriodTu;
    bcn->timestamp = 0;
    bcn->cap.set_ess(1);
    bcn->cap.set_short_preamble(1);

    BufferWriter elem_w({bcn->elements, w.RemainingBytes()});
    common::WriteSsid(&elem_w, kSsid);

    RatesWriter rates_writer{kSupportedRates};
    rates_writer.WriteSupportedRates(&elem_w);
    common::WriteDsssParamSet(&elem_w, kBssChannel.primary);
    WriteCountry(&elem_w, kBssChannel);
    rates_writer.WriteExtendedSupportedRates(&elem_w);

    ZX_DEBUG_ASSERT(bcn->Validate(elem_w.WrittenBytes()));
    packet->set_len(w.WrittenBytes() + elem_w.WrittenBytes());

    wlan_rx_info_t rx_info{.rx_flags = 0};
    packet->CopyCtrlFrom(rx_info);

    *out_packet = fbl::move(packet);

    return ZX_OK;
}

zx_status_t CreateProbeRequest(fbl::unique_ptr<Packet>* out_packet) {
    common::MacAddr bssid(kBssid1);
    common::MacAddr client(kClientAddress);

    constexpr size_t ie_len = 256;
    constexpr size_t max_frame_len = MgmtFrameHeader::max_len() + ProbeRequest::max_len() + ie_len;
    auto packet = GetWlanPacket(max_frame_len);
    if (packet == nullptr) { return ZX_ERR_NO_RESOURCES; }

    BufferWriter w(*packet);
    auto mgmt_hdr = w.Write<MgmtFrameHeader>();
    mgmt_hdr->fc.set_type(FrameType::kManagement);
    mgmt_hdr->fc.set_subtype(ManagementSubtype::kBeacon);
    mgmt_hdr->addr1 = client;
    mgmt_hdr->addr2 = bssid;
    mgmt_hdr->addr3 = bssid;

    auto probereq = w.Write<ProbeRequest>();
    BufferWriter elem_w({probereq->elements, w.RemainingBytes()});
    common::WriteSsid(&elem_w, kSsid);

    RatesWriter rates_writer{kSupportedRates};
    rates_writer.WriteSupportedRates(&elem_w);
    rates_writer.WriteExtendedSupportedRates(&elem_w);
    common::WriteDsssParamSet(&elem_w, kBssChannel.primary);

    ZX_DEBUG_ASSERT(probereq->Validate(elem_w.WrittenBytes()));
    packet->set_len(w.WrittenBytes() + elem_w.WrittenBytes());

    wlan_rx_info_t rx_info{.rx_flags = 0};
    packet->CopyCtrlFrom(rx_info);

    *out_packet = fbl::move(packet);

    return ZX_OK;
}

zx_status_t CreateAuthReqFrame(fbl::unique_ptr<Packet>* out_packet) {
    common::MacAddr bssid(kBssid1);
    common::MacAddr client(kClientAddress);
    constexpr size_t max_frame_len = MgmtFrameHeader::max_len() + Authentication::max_len();
    auto packet = GetWlanPacket(max_frame_len);
    if (packet == nullptr) { return ZX_ERR_NO_RESOURCES; }

    BufferWriter w(*packet);
    auto mgmt_hdr = w.Write<MgmtFrameHeader>();
    mgmt_hdr->fc.set_type(FrameType::kManagement);
    mgmt_hdr->fc.set_subtype(ManagementSubtype::kAuthentication);
    mgmt_hdr->addr1 = bssid;
    mgmt_hdr->addr2 = client;
    mgmt_hdr->addr3 = bssid;

    auto auth = w.Write<Authentication>();
    auth->auth_algorithm_number = AuthAlgorithm::kOpenSystem;
    auth->auth_txn_seq_number = 1;
    auth->status_code = 0;  // Reserved: explicitly set to 0

    packet->set_len(w.WrittenBytes());

    wlan_rx_info_t rx_info{.rx_flags = 0};
    packet->CopyCtrlFrom(rx_info);

    *out_packet = fbl::move(packet);

    return ZX_OK;
}

zx_status_t CreateAuthRespFrame(fbl::unique_ptr<Packet>* out_packet) {
    common::MacAddr bssid(kBssid1);
    common::MacAddr client(kClientAddress);

    constexpr size_t max_frame_len = MgmtFrameHeader::max_len() + Authentication::max_len();
    auto packet = GetWlanPacket(max_frame_len);
    if (packet == nullptr) { return ZX_ERR_NO_RESOURCES; }

    BufferWriter w(*packet);
    auto mgmt_hdr = w.Write<MgmtFrameHeader>();
    mgmt_hdr->fc.set_type(FrameType::kManagement);
    mgmt_hdr->fc.set_subtype(ManagementSubtype::kAuthentication);
    mgmt_hdr->addr1 = client;
    mgmt_hdr->addr2 = bssid;
    mgmt_hdr->addr3 = bssid;

    auto auth = w.Write<Authentication>();
    auth->auth_algorithm_number = AuthAlgorithm::kOpenSystem;
    auth->auth_txn_seq_number = 2;
    auth->status_code = status_code::kSuccess;

    packet->set_len(w.WrittenBytes());

    wlan_rx_info_t rx_info{.rx_flags = 0};
    packet->CopyCtrlFrom(rx_info);

    *out_packet = fbl::move(packet);

    return ZX_OK;
}

zx_status_t CreateDeauthFrame(fbl::unique_ptr<Packet>* out_packet) {
    common::MacAddr bssid(kBssid1);
    common::MacAddr client(kClientAddress);

    constexpr size_t max_frame_len = MgmtFrameHeader::max_len() + Deauthentication::max_len();
    auto packet = GetWlanPacket(max_frame_len);
    if (packet == nullptr) { return ZX_ERR_NO_RESOURCES; }

    BufferWriter w(*packet);
    auto mgmt_hdr = w.Write<MgmtFrameHeader>();
    mgmt_hdr->fc.set_type(FrameType::kManagement);
    mgmt_hdr->fc.set_subtype(ManagementSubtype::kDeauthentication);
    mgmt_hdr->addr1 = bssid;
    mgmt_hdr->addr2 = client;
    mgmt_hdr->addr3 = bssid;

    w.Write<Deauthentication>()->reason_code = reason_code::ReasonCode::kLeavingNetworkDeauth;

    packet->set_len(w.WrittenBytes());

    wlan_rx_info_t rx_info{.rx_flags = 0};
    packet->CopyCtrlFrom(rx_info);

    *out_packet = fbl::move(packet);

    return ZX_OK;
}

zx_status_t CreateAssocReqFrame(fbl::unique_ptr<Packet>* out_packet, Span<const uint8_t> ssid,
                                bool rsn) {
    common::MacAddr bssid(kBssid1);
    common::MacAddr client(kClientAddress);

    // arbitrarily large reserved len; will shrink down later
    constexpr size_t ie_len = 1024;
    constexpr size_t max_frame_len =
        MgmtFrameHeader::max_len() + AssociationRequest::max_len() + ie_len;
    auto packet = GetWlanPacket(max_frame_len);
    if (packet == nullptr) { return ZX_ERR_NO_RESOURCES; }

    BufferWriter w(*packet);
    auto mgmt_hdr = w.Write<MgmtFrameHeader>();
    mgmt_hdr->fc.set_type(FrameType::kManagement);
    mgmt_hdr->fc.set_subtype(ManagementSubtype::kAssociationRequest);
    mgmt_hdr->addr1 = bssid;
    mgmt_hdr->addr2 = client;
    mgmt_hdr->addr3 = bssid;

    auto assoc = w.Write<AssociationRequest>();
    CapabilityInfo cap = {};
    cap.set_short_preamble(1);
    cap.set_ess(1);
    assoc->cap = cap;
    assoc->listen_interval = kListenInterval;

    BufferWriter elem_w({assoc->elements, w.RemainingBytes()});
    if (!ssid.empty()) { common::WriteSsid(&w, ssid); }
    if (rsn) { w.Write(kRsne); }

    packet->set_len(w.WrittenBytes() + elem_w.WrittenBytes());

    wlan_rx_info_t rx_info{.rx_flags = 0};
    packet->CopyCtrlFrom(rx_info);

    *out_packet = fbl::move(packet);

    return ZX_OK;
}

zx_status_t CreateAssocRespFrame(fbl::unique_ptr<Packet>* out_packet) {
    common::MacAddr bssid(kBssid1);
    common::MacAddr client(kClientAddress);

    constexpr size_t max_frame_len = MgmtFrameHeader::max_len() + AssociationResponse::max_len();
    auto packet = GetWlanPacket(max_frame_len);
    if (packet == nullptr) { return ZX_ERR_NO_RESOURCES; }

    BufferWriter w(*packet);
    auto mgmt_hdr = w.Write<MgmtFrameHeader>();
    mgmt_hdr->fc.set_type(FrameType::kManagement);
    mgmt_hdr->fc.set_subtype(ManagementSubtype::kAssociationResponse);
    mgmt_hdr->addr1 = client;
    mgmt_hdr->addr2 = bssid;
    mgmt_hdr->addr3 = bssid;

    auto assoc = w.Write<AssociationResponse>();
    assoc->aid = kAid;
    CapabilityInfo cap = {};
    cap.set_short_preamble(1);
    cap.set_ess(1);
    assoc->cap = cap;
    assoc->status_code = status_code::kSuccess;

    packet->set_len(w.WrittenBytes());

    wlan_rx_info_t rx_info{.rx_flags = 0};
    packet->CopyCtrlFrom(rx_info);

    *out_packet = fbl::move(packet);

    return ZX_OK;
}

zx_status_t CreateDisassocFrame(fbl::unique_ptr<Packet>* out_packet) {
    common::MacAddr bssid(kBssid1);
    common::MacAddr client(kClientAddress);

    constexpr size_t max_frame_len = MgmtFrameHeader::max_len() + Disassociation::max_len();
    auto packet = GetWlanPacket(max_frame_len);
    if (packet == nullptr) { return ZX_ERR_NO_RESOURCES; }

    BufferWriter w(*packet);
    auto mgmt_hdr = w.Write<MgmtFrameHeader>();
    mgmt_hdr->fc.set_type(FrameType::kManagement);
    mgmt_hdr->fc.set_subtype(ManagementSubtype::kDisassociation);
    mgmt_hdr->addr1 = bssid;
    mgmt_hdr->addr2 = client;
    mgmt_hdr->addr3 = bssid;

    w.Write<Disassociation>()->reason_code = reason_code::ReasonCode::kLeavingNetworkDisassoc;

    packet->set_len(w.WrittenBytes());

    wlan_rx_info_t rx_info{.rx_flags = 0};
    packet->CopyCtrlFrom(rx_info);

    *out_packet = fbl::move(packet);

    return ZX_OK;
}

DataFrame<LlcHeader> CreateDataFrame(const uint8_t* payload, size_t len) {
    common::MacAddr bssid(kBssid1);
    common::MacAddr client(kClientAddress);

    const size_t buf_len = DataFrameHeader::max_len() + LlcHeader::max_len() + len;
    auto packet = GetWlanPacket(buf_len);
    if (packet == nullptr) { return {}; }

    BufferWriter w(*packet);
    auto data_hdr = w.Write<DataFrameHeader>();
    data_hdr->fc.set_type(FrameType::kData);
    data_hdr->fc.set_subtype(DataSubtype::kDataSubtype);
    data_hdr->fc.set_from_ds(1);
    data_hdr->addr1 = bssid;
    data_hdr->addr2 = bssid;
    data_hdr->addr3 = client;
    data_hdr->sc.set_val(42);

    auto llc_hdr = w.Write<LlcHeader>();
    llc_hdr->dsap = kLlcSnapExtension;
    llc_hdr->ssap = kLlcSnapExtension;
    llc_hdr->control = kLlcUnnumberedInformation;
    std::memcpy(llc_hdr->oui, kLlcOui, sizeof(llc_hdr->oui));
    llc_hdr->protocol_id = 42;
    if (len > 0) { w.Write({payload, len}); }

    packet->set_len(w.WrittenBytes());

    wlan_rx_info_t rx_info{.rx_flags = 0};
    packet->CopyCtrlFrom(rx_info);

    return DataFrame<LlcHeader>(fbl::move(packet));
}

DataFrame<> CreateNullDataFrame() {
    common::MacAddr bssid(kBssid1);
    common::MacAddr client(kClientAddress);

    auto packet = GetWlanPacket(DataFrameHeader::max_len());
    if (packet == nullptr) { return {}; }

    BufferWriter w(*packet);
    auto data_hdr = w.Write<DataFrameHeader>();
    data_hdr->fc.set_type(FrameType::kData);
    data_hdr->fc.set_subtype(DataSubtype::kNull);
    data_hdr->fc.set_from_ds(1);
    data_hdr->addr1 = client;
    data_hdr->addr2 = bssid;
    data_hdr->addr3 = bssid;
    data_hdr->sc.set_val(42);

    packet->set_len(w.WrittenBytes());

    wlan_rx_info_t rx_info{.rx_flags = 0};
    packet->CopyCtrlFrom(rx_info);

    return DataFrame<>(fbl::move(packet));
}

EthFrame CreateEthFrame(const uint8_t* payload, size_t len) {
    common::MacAddr bssid(kBssid1);
    common::MacAddr client(kClientAddress);

    size_t buf_len = EthernetII::max_len() + len;
    auto packet = GetEthPacket(buf_len);
    if (packet == nullptr) { return {}; }

    BufferWriter w(*packet);
    auto eth_hdr = w.Write<EthernetII>();
    eth_hdr->src = client;
    eth_hdr->dest = bssid;
    eth_hdr->ether_type = 2;
    w.Write({payload, len});

    return EthFrame(fbl::move(packet));
}

}  // namespace wlan
