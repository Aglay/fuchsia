// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <wlan/mlme/ap/infra_bss.h>

#include <wlan/common/channel.h>
#include <wlan/mlme/debug.h>
#include <wlan/mlme/key.h>
#include <wlan/mlme/mac_frame.h>
#include <wlan/mlme/mlme.h>
#include <wlan/mlme/packet.h>
#include <wlan/mlme/service.h>

#include <zircon/syscalls.h>

namespace wlan {

namespace wlan_mlme = ::fuchsia::wlan::mlme;

InfraBss::InfraBss(DeviceInterface* device, fbl::unique_ptr<BeaconSender> bcn_sender,
                   const common::MacAddr& bssid)
    : bssid_(bssid), device_(device), bcn_sender_(fbl::move(bcn_sender)) {
    ZX_DEBUG_ASSERT(bcn_sender_ != nullptr);
}

InfraBss::~InfraBss() {
    // The BSS should always be explicitly stopped.
    // Throw in debug builds, stop in release ones.
    ZX_DEBUG_ASSERT(!IsStarted());

    // Ensure BSS is stopped correctly.
    Stop();
}

void InfraBss::Start(const MlmeMsg<wlan_mlme::StartRequest>& req) {
    if (IsStarted()) { return; }

    // Move to requested channel.
    auto chan = wlan_channel_t{
        .primary = req.body()->channel,
        .cbw = CBW20,
    };

    if (Ht().cbw_40_rx_ready) {
        wlan_channel_t chan_override = chan;
        chan_override.cbw = CBW40;
        chan.cbw = common::GetValidCbw(chan_override);
    }

    auto status = device_->SetChannel(chan);
    if (status != ZX_OK) {
        errorf("[infra-bss] [%s] requested start on channel %u failed: %d\n",
               bssid_.ToString().c_str(), req.body()->channel, status);
    }
    chan_ = chan;

    ZX_DEBUG_ASSERT(req.body()->dtim_period > 0);
    if (req.body()->dtim_period == 0) {
        ps_cfg_.SetDtimPeriod(1);
        warnf(
            "[infra-bss] [%s] received start request with reserved DTIM period of "
            "0; falling back "
            "to DTIM period of 1\n",
            bssid_.ToString().c_str());
    } else {
        ps_cfg_.SetDtimPeriod(req.body()->dtim_period);
    }

    debugbss("[infra-bss] [%s] starting BSS\n", bssid_.ToString().c_str());
    debugbss("    SSID: \"%s\"\n", debug::ToAsciiOrHexStr(*req.body()->ssid).c_str());
    debugbss("    Beacon Period: %u\n", req.body()->beacon_period);
    debugbss("    DTIM Period: %u\n", req.body()->dtim_period);
    debugbss("    Channel: %u\n", req.body()->channel);

    // Keep track of start request which holds important configuration
    // information.
    req.body()->Clone(&start_req_);

    // Start sending Beacon frames.
    started_at_ = zx_clock_get(ZX_CLOCK_MONOTONIC);
    bcn_sender_->Start(this, ps_cfg_, req);
}

void InfraBss::Stop() {
    if (!IsStarted()) { return; }

    debugbss("[infra-bss] [%s] stopping BSS\n", bssid_.ToString().c_str());

    clients_.clear();
    bcn_sender_->Stop();
    started_at_ = 0;
}

bool InfraBss::IsStarted() {
    return started_at_ != 0;
}

void InfraBss::HandleAnyFrame(fbl::unique_ptr<Packet> pkt) {
    switch (pkt->peer()) {
    case Packet::Peer::kEthernet: {
        if (auto eth_frame = EthFrameView::CheckType(pkt.get()).CheckLength()) {
            HandleEthFrame(eth_frame.IntoOwned(fbl::move(pkt)));
        }
        break;
    }
    case Packet::Peer::kWlan:
        HandleAnyWlanFrame(fbl::move(pkt));
        break;
    default:
        errorf("unknown Packet peer: %u\n", pkt->peer());
        break;
    }
}

void InfraBss::HandleAnyWlanFrame(fbl::unique_ptr<Packet> pkt) {
    if (auto possible_mgmt_frame = MgmtFrameView<>::CheckType(pkt.get())) {
        if (auto mgmt_frame = possible_mgmt_frame.CheckLength()) {
            HandleAnyMgmtFrame(mgmt_frame.IntoOwned(fbl::move(pkt)));
        }
    } else if (auto possible_data_frame = DataFrameView<>::CheckType(pkt.get())) {
        if (auto data_frame = possible_data_frame.CheckLength()) {
            HandleAnyDataFrame(data_frame.IntoOwned(fbl::move(pkt)));
        }
    } else if (auto possible_ctrl_frame = CtrlFrameView<>::CheckType(pkt.get())) {
        if (auto ctrl_frame = possible_ctrl_frame.CheckLength()) {
            HandleAnyCtrlFrame(ctrl_frame.IntoOwned(fbl::move(pkt)));
        }
    }
}

void InfraBss::HandleAnyMgmtFrame(MgmtFrame<>&& frame) {
    auto mgmt_frame = frame.View();
    bool to_bss = (bssid_ == mgmt_frame.hdr()->addr1 && bssid_ == mgmt_frame.hdr()->addr3);

    // Special treatment for ProbeRequests which can be addressed towards
    // broadcast address.
    if (auto possible_probe_req_frame = mgmt_frame.CheckBodyType<ProbeRequest>()) {
        if (auto probe_req_frame = possible_probe_req_frame.CheckLength()) {
            // Drop all ProbeRequests which are neither targeted to this BSS nor to
            // broadcast address.
            auto hdr = probe_req_frame.hdr();
            bool to_bcast = hdr->addr1.IsBcast() && hdr->addr3.IsBcast();
            if (!to_bss && !to_bcast) { return; }

            // Valid ProbeRequest, let BeaconSender process and respond to it.
            bcn_sender_->SendProbeResponse(probe_req_frame);
            return;
        }
    }

    // Drop management frames which are not targeted towards this BSS.
    if (!to_bss) { return; }

    // Register the client if it's not yet known.
    const auto& client_addr = mgmt_frame.hdr()->addr2;
    if (!HasClient(client_addr)) {
        if (auto auth_frame = mgmt_frame.CheckBodyType<Authentication>().CheckLength()) {
            HandleNewClientAuthAttempt(auth_frame);
        }
    }

    // Forward all frames to the correct client.
    auto client = GetClient(client_addr);
    if (client != nullptr) { client->HandleAnyMgmtFrame(fbl::move(frame)); }
}

void InfraBss::HandleAnyDataFrame(DataFrame<>&& frame) {
    if (bssid_ != frame.hdr()->addr1) { return; }

    // Let the correct RemoteClient instance process the received frame.
    const auto& client_addr = frame.hdr()->addr2;
    auto client = GetClient(client_addr);
    if (client != nullptr) { client->HandleAnyDataFrame(fbl::move(frame)); }
}

void InfraBss::HandleAnyCtrlFrame(CtrlFrame<>&& frame) {
    auto ctrl_frame = frame.View();

    if (auto pspoll_frame = ctrl_frame.CheckBodyType<PsPollFrame>().CheckLength()) {
        if (pspoll_frame.body()->bssid != bssid_) { return; }

        const auto& client_addr = pspoll_frame.body()->ta;
        auto client = GetClient(client_addr);
        if (client == nullptr) { return; }

        client->HandleAnyCtrlFrame(fbl::move(frame));
    }
}

zx_status_t InfraBss::HandleTimeout(const common::MacAddr& client_addr) {
    auto client = GetClient(client_addr);
    ZX_DEBUG_ASSERT(client != nullptr);
    if (client != nullptr) { client->HandleTimeout(); }
    return ZX_OK;
}

void InfraBss::HandleEthFrame(EthFrame&& eth_frame) {
    // Lookup client associated with incoming unicast frame.
    auto& dest_addr = eth_frame.hdr()->dest;
    if (dest_addr.IsUcast()) {
        auto client = GetClient(dest_addr);
        if (client != nullptr) { client->HandleAnyEthFrame(fbl::move(eth_frame)); }
    } else {
        // Process multicast frames ourselves.
        if (auto data_frame = EthToDataFrame(eth_frame, false)) {
            SendDataFrame(data_frame->Generalize());
        } else {
            errorf("[infra-bss] [%s] couldn't convert ethernet frame\n", bssid_.ToString().c_str());
        }
    }
}

zx_status_t InfraBss::HandleMlmeMsg(const BaseMlmeMsg& msg) {
    if (auto set_keys_req = msg.As<wlan_mlme::SetKeysRequest>()) {
        return HandleMlmeSetKeysReq(*set_keys_req);
    }

    auto peer_addr = service::GetPeerAddr(msg);
    if (!peer_addr.has_value()) {
        warnf("[infra-bss] received unsupported MLME msg; ordinal: %u\n", msg.ordinal());
        return ZX_ERR_INVALID_ARGS;
    }

    if (auto client = GetClient(peer_addr.value())) {
        return client->HandleMlmeMsg(msg);
    } else {
        warnf("[infra-bss] unrecognized peer address in MlmeMsg: %s -- ordinal: %u\n",
              peer_addr.value().ToString().c_str(), msg.ordinal());
    }

    return ZX_OK;
}

void InfraBss::HandleNewClientAuthAttempt(const MgmtFrameView<Authentication>& frame) {
    auto& client_addr = frame.hdr()->addr2;
    ZX_DEBUG_ASSERT(!HasClient(client_addr));

    debugbss("[infra-bss] [%s] new client: %s\n", bssid_.ToString().c_str(),
             client_addr.ToString().c_str());

    // Else, create a new remote client instance.
    fbl::unique_ptr<Timer> timer = nullptr;
    auto status = CreateClientTimer(client_addr, &timer);
    if (status == ZX_OK) {
        auto client = fbl::make_unique<RemoteClient>(device_, fbl::move(timer),
                                                     this,  // bss
                                                     this,  // client listener
                                                     client_addr);
        clients_.emplace(client_addr, fbl::move(client));
    } else {
        errorf("[infra-bss] [%s] could not create client timer: %d\n", bssid_.ToString().c_str(),
               status);
    }
}

zx_status_t InfraBss::HandleMlmeSetKeysReq(const MlmeMsg<wlan_mlme::SetKeysRequest>& req) {
    debugfn();

    if (!IsRsn()) {
        warnf("[infra-bss] ignoring SetKeysRequest since AP is unprotected\n");
        return ZX_ERR_BAD_STATE;
    }

    for (auto& key_desc : *req.body()->keylist) {
        auto key_config = ToKeyConfig(key_desc);
        if (!key_config.has_value()) { return ZX_ERR_NOT_SUPPORTED; }

        auto status = device_->SetKey(&key_config.value());
        if (status != ZX_OK) {
            errorf("Could not configure keys in hardware: %d\n", status);
            return status;
        }

        common::MacAddr client_addr(key_desc.address.data());
        if (client_addr.IsUcast()) {
            auto client = GetClient(client_addr);
            // TODO: We should only open controlled port when RSNA has been established, not as soon
            //       as one key has. Going with simple logic for now to get things working, but this
            //       should be revisited later. Specifically, we should have an MLME primitive to
            //       open a controlled port since the SME has all the information about when all
            //       the keys finish deriving.
            if (client != nullptr) { client->OpenControlledPort(); }
        }
    }

    return ZX_OK;
}

zx_status_t InfraBss::HandleClientDeauth(const common::MacAddr& client_addr) {
    debugfn();
    auto iter = clients_.find(client_addr);
    ZX_DEBUG_ASSERT(iter != clients_.end());
    if (iter == clients_.end()) {
        errorf("[infra-bss] [%s] unknown client deauthenticated: %s\n", bssid_.ToString().c_str(),
               client_addr.ToString().c_str());
        return ZX_ERR_BAD_STATE;
    }

    debugbss("[infra-bss] [%s] removing client %s\n", bssid_.ToString().c_str(),
             client_addr.ToString().c_str());
    clients_.erase(iter);
    return ZX_OK;
}

void InfraBss::HandleClientDisassociation(aid_t aid) {
    debugfn();
    ps_cfg_.GetTim()->SetTrafficIndication(aid, false);
}

void InfraBss::HandleClientBuChange(const common::MacAddr& client_addr, aid_t aid,
                                    size_t bu_count) {
    debugfn();
    auto client = GetClient(client_addr);
    ZX_DEBUG_ASSERT(client != nullptr);
    if (client == nullptr) {
        errorf("[infra-bss] [%s] received traffic indication for untracked client: %s\n",
               bssid_.ToString().c_str(), client_addr.ToString().c_str());
        return;
    }
    ZX_DEBUG_ASSERT(aid != kUnknownAid);
    if (aid == kUnknownAid) {
        errorf(
            "[infra-bss] [%s] received traffic indication from client with unknown "
            "AID: %s\n",
            bssid_.ToString().c_str(), client_addr.ToString().c_str());
        return;
    }

    ps_cfg_.GetTim()->SetTrafficIndication(aid, bu_count > 0);
}

bool InfraBss::HasClient(const common::MacAddr& client) {
    return clients_.find(client) != clients_.end();
}

RemoteClientInterface* InfraBss::GetClient(const common::MacAddr& addr) {
    auto iter = clients_.find(addr);
    if (iter == clients_.end()) { return nullptr; }
    return iter->second.get();
}

zx_status_t InfraBss::CreateClientTimer(const common::MacAddr& client_addr,
                                        fbl::unique_ptr<Timer>* out_timer) {
    ObjectId timer_id;
    timer_id.set_subtype(to_enum_type(ObjectSubtype::kTimer));
    timer_id.set_target(to_enum_type(ObjectTarget::kBss));
    timer_id.set_mac(client_addr.ToU64());
    zx_status_t status =
        device_->GetTimer(ToPortKey(PortKeyType::kMlme, timer_id.val()), out_timer);
    if (status != ZX_OK) {
        errorf("[infra-bss] [%s] could not create bss timer: %d\n", bssid_.ToString().c_str(),
               status);
        return status;
    }
    return ZX_OK;
}

bool InfraBss::ShouldBufferFrame(const common::MacAddr& receiver_addr) const {
    // Buffer non-GCR-SP frames when at least one client is dozing.
    // Note: Currently group addressed service transmission is not supported and
    // thus, every group message should get buffered.
    return receiver_addr.IsGroupAddr() && ps_cfg_.GetTim()->HasDozingClients();
}

zx_status_t InfraBss::BufferFrame(fbl::unique_ptr<Packet> packet) {
    // Drop oldest frame if queue reached its limit.
    if (bu_queue_.size() >= kMaxGroupAddressedBu) {
        bu_queue_.pop();
        warnf("[infra-bss] [%s] dropping oldest group addressed frame\n",
              bssid_.ToString().c_str());
    }

    debugps("[infra-bss] [%s] buffer outbound frame\n", bssid_.ToString().c_str());
    bu_queue_.push(fbl::move(packet));
    ps_cfg_.GetTim()->SetTrafficIndication(kGroupAdressedAid, true);
    return ZX_OK;
}

zx_status_t InfraBss::SendDataFrame(DataFrame<>&& data_frame) {
    if (ShouldBufferFrame(data_frame.hdr()->addr1)) { return BufferFrame(data_frame.Take()); }

    return device_->SendWlan(data_frame.Take());
}

zx_status_t InfraBss::SendMgmtFrame(MgmtFrame<>&& mgmt_frame) {
    if (ShouldBufferFrame(mgmt_frame.hdr()->addr1)) { return BufferFrame(mgmt_frame.Take()); }

    return device_->SendWlan(mgmt_frame.Take());
}

zx_status_t InfraBss::SendEthFrame(EthFrame&& eth_frame) {
    return device_->SendEthernet(eth_frame.Take());
}

zx_status_t InfraBss::SendNextBu() {
    ZX_DEBUG_ASSERT(bu_queue_.size() > 0);
    if (bu_queue_.empty()) { return ZX_ERR_BAD_STATE; }

    auto packet = std::move(bu_queue_.front());
    bu_queue_.pop();

    if (auto fc = packet->mut_field<FrameControl>(0)) {
        // Set `more` bit if there are more BU available.
        // IEEE Std 802.11-2016, 9.2.4.1.8
        fc->set_more_data(bu_queue_.size() > 0);
        debugps("[infra-bss] [%s] sent group addressed BU\n", bssid_.ToString().c_str());
        return device_->SendWlan(fbl::move(packet));
    } else {
        return ZX_ERR_BUFFER_TOO_SMALL;
    }
}

std::optional<DataFrame<LlcHeader>> InfraBss::EthToDataFrame(const EthFrame& eth_frame,
                                                             bool needs_protection) {
    size_t payload_len = eth_frame.body_len();
    size_t max_frame_len = DataFrameHeader::max_len() + LlcHeader::max_len() + payload_len;
    auto packet = GetWlanPacket(max_frame_len);
    if (packet == nullptr) {
        errorf("[infra-bss] [%s] cannot convert ethernet to data frame: out of packets (%zu)\n",
               bssid_.ToString().c_str(), max_frame_len);
        return std::nullopt;
    }
    packet->clear();

    DataFrame<LlcHeader> data_frame(fbl::move(packet));
    auto data_hdr = data_frame.hdr();
    data_hdr->fc.set_type(FrameType::kData);
    data_hdr->fc.set_subtype(DataSubtype::kDataSubtype);
    data_hdr->fc.set_from_ds(1);
    data_hdr->fc.set_protected_frame(needs_protection ? 1 : 0);
    data_hdr->addr1 = eth_frame.hdr()->dest;
    data_hdr->addr2 = bssid_;
    data_hdr->addr3 = eth_frame.hdr()->src;
    data_hdr->sc.set_seq(NextSeq(*data_hdr));

    auto llc_hdr = data_frame.body();
    llc_hdr->dsap = kLlcSnapExtension;
    llc_hdr->ssap = kLlcSnapExtension;
    llc_hdr->control = kLlcUnnumberedInformation;
    std::memcpy(llc_hdr->oui, kLlcOui, sizeof(llc_hdr->oui));
    llc_hdr->protocol_id = eth_frame.hdr()->ether_type;
    std::memcpy(llc_hdr->payload, eth_frame.body(), payload_len);

    size_t actual_body_len = llc_hdr->len() + payload_len;
    auto status = data_frame.set_body_len(actual_body_len);
    if (status != ZX_OK) {
        errorf("[infra-bss] [%s] could not set data body length to %zu: %d\n",
               bssid_.ToString().c_str(), actual_body_len, status);
        return std::nullopt;
    }

    finspect("Outbound data frame: len %zu, hdr_len:%zu body_len:%zu\n", data_frame.len(),
             data_hdr->len(), llc_hdr->len());
    finspect("  wlan hdr: %s\n", debug::Describe(*data_hdr).c_str());
    finspect("  llc  hdr: %s\n", debug::Describe(*llc_hdr).c_str());
    finspect("  frame   : %s\n",
             debug::HexDump(data_frame.View().data(), data_frame.len()).c_str());

    // Ralink appears to setup BlockAck session AND AMPDU handling
    // TODO(porce): Use a separate sequence number space in that case
    CBW cbw = CBW20;
    if (Ht().cbw_40_tx_ready && eth_frame.hdr()->src.IsUcast()) {
        // 40MHz direction does not matter here.
        // Radio uses the operational channel setting. This indicates the
        // bandwidth without direction.
        cbw = CBW40;
    }
    data_frame.FillTxInfo(cbw, WLAN_PHY_HT);
    return std::make_optional(fbl::move(data_frame));
}

void InfraBss::OnPreTbtt() {
    bcn_sender_->UpdateBeacon(ps_cfg_);
    ps_cfg_.NextDtimCount();
}

void InfraBss::OnBcnTxComplete() {
    // Only send out multicast frames if the Beacon we just sent was a DTIM.
    if (ps_cfg_.LastDtimCount() != 0) { return; }
    if (bu_queue_.size() == 0) { return; }

    debugps("[infra-bss] [%s] sending %zu group addressed BU\n", bssid_.ToString().c_str(),
            bu_queue_.size());
    while (bu_queue_.size() > 0) {
        auto status = SendNextBu();
        if (status != ZX_OK) {
            errorf("[infra-bss] [%s] could not send group addressed BU: %d\n",
                   bssid_.ToString().c_str(), status);
            return;
        }
    }

    ps_cfg_.GetTim()->SetTrafficIndication(kGroupAdressedAid, false);
}

const common::MacAddr& InfraBss::bssid() const {
    return bssid_;
}

uint64_t InfraBss::timestamp() {
    zx_time_t now = zx_clock_get(ZX_CLOCK_MONOTONIC);
    zx_duration_t uptime_ns = now - started_at_;
    return uptime_ns / 1000;  // as microseconds
}

seq_t InfraBss::NextSeq(const MgmtFrameHeader& hdr) {
    return NextSeqNo(hdr, &seq_);
}

seq_t InfraBss::NextSeq(const MgmtFrameHeader& hdr, uint8_t aci) {
    return NextSeqNo(hdr, aci, &seq_);
}

seq_t InfraBss::NextSeq(const DataFrameHeader& hdr) {
    return NextSeqNo(hdr, &seq_);
}

bool InfraBss::IsRsn() const {
    return !start_req_.rsne.is_null();
}

HtConfig InfraBss::Ht() const {
    // TODO(NET-567): Reflect hardware capabilities and association negotiation
    return HtConfig {
        .ready = true,
        .cbw_40_rx_ready = true,
        .cbw_40_tx_ready = false,
    };
}

}  // namespace wlan
