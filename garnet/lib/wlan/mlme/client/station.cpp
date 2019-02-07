// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <wlan/mlme/client/station.h>

#include <garnet/lib/rust/wlan-mlme-c/bindings.h>
#include <wlan/common/band.h>
#include <wlan/common/buffer_writer.h>
#include <wlan/common/channel.h>
#include <wlan/common/energy.h>
#include <wlan/common/logging.h>
#include <wlan/common/stats.h>
#include <wlan/common/tim_element.h>
#include <wlan/common/write_element.h>
#include <wlan/mlme/client/bss.h>
#include <wlan/mlme/client/client_mlme.h>
#include <wlan/mlme/debug.h>
#include <wlan/mlme/device_interface.h>
#include <wlan/mlme/key.h>
#include <wlan/mlme/mac_frame.h>
#include <wlan/mlme/packet.h>
#include <wlan/mlme/rates_elements.h>
#include <wlan/mlme/sequence.h>
#include <wlan/mlme/service.h>

#include <fuchsia/wlan/mlme/c/fidl.h>
#include <zircon/status.h>

#include <inttypes.h>
#include <algorithm>
#include <cstring>
#include <utility>

namespace wlan {

namespace wlan_mlme = ::fuchsia::wlan::mlme;
namespace wlan_stats = ::fuchsia::wlan::stats;
using common::dBm;

// TODO(hahnr): Revisit frame construction to reduce boilerplate code.

Station::Station(DeviceInterface* device, TimerManager<>&& timer_mgr, ChannelScheduler* chan_sched,
                 JoinContext* join_ctx)
    : device_(device),
      timer_mgr_(std::move(timer_mgr)),
      chan_sched_(chan_sched),
      join_ctx_(join_ctx) {
    Reset();
}

void Station::Reset() {
    debugfn();

    state_ = WlanState::kIdle;
    timer_mgr_.CancelAll();
    bu_queue_.clear();
}

zx_status_t Station::HandleWlanFrame(fbl::unique_ptr<Packet> pkt) {
    ZX_DEBUG_ASSERT(pkt->peer() == Packet::Peer::kWlan);
    WLAN_STATS_INC(rx_frame.in);
    WLAN_STATS_ADD(pkt->len(), rx_frame.in_bytes);

    if (auto possible_mgmt_frame = MgmtFrameView<>::CheckType(pkt.get())) {
        auto mgmt_frame = possible_mgmt_frame.CheckLength();
        if (!mgmt_frame) { return ZX_ERR_BUFFER_TOO_SMALL; }

        HandleMgmtFrame(mgmt_frame.IntoOwned(std::move(pkt)));
    } else if (auto possible_data_frame = DataFrameView<>::CheckType(pkt.get())) {
        auto data_frame = possible_data_frame.CheckLength();
        if (!data_frame) { return ZX_ERR_BUFFER_TOO_SMALL; }

        HandleDataFrame(data_frame.IntoOwned(std::move(pkt)));
    }

    return ZX_OK;
}

zx_status_t Station::HandleMgmtFrame(MgmtFrame<>&& frame) {
    auto mgmt_frame = frame.View();

    WLAN_STATS_INC(mgmt_frame.in);
    if (ShouldDropMgmtFrame(mgmt_frame)) {
        WLAN_STATS_INC(mgmt_frame.drop);
        return ZX_ERR_NOT_SUPPORTED;
    }
    WLAN_STATS_INC(mgmt_frame.out);

    if (auto possible_bcn_frame = mgmt_frame.CheckBodyType<Beacon>()) {
        if (auto bcn_frame = possible_bcn_frame.CheckLength()) {
            HandleBeacon(bcn_frame.IntoOwned(frame.Take()));
        }
    } else if (auto possible_auth_frame = mgmt_frame.CheckBodyType<Authentication>()) {
        if (auto auth_frame = possible_auth_frame.CheckLength()) {
            HandleAuthentication(auth_frame.IntoOwned(frame.Take()));
        }
    } else if (auto possible_deauth_frame = mgmt_frame.CheckBodyType<Deauthentication>()) {
        if (auto deauth_frame = possible_deauth_frame.CheckLength()) {
            HandleDeauthentication(deauth_frame.IntoOwned(frame.Take()));
        }
    } else if (auto possible_assoc_resp_frame = mgmt_frame.CheckBodyType<AssociationResponse>()) {
        if (auto assoc_resp_frame = possible_assoc_resp_frame.CheckLength()) {
            HandleAssociationResponse(assoc_resp_frame.IntoOwned(frame.Take()));
        }
    } else if (auto possible_disassoc_frame = mgmt_frame.CheckBodyType<Disassociation>()) {
        if (auto disassoc_frame = possible_disassoc_frame.CheckLength()) {
            HandleDisassociation(disassoc_frame.IntoOwned(frame.Take()));
        }
    } else if (auto possible_action_frame = mgmt_frame.CheckBodyType<ActionFrame>()) {
        if (auto action_frame = possible_action_frame.CheckLength()) {
            HandleActionFrame(action_frame.IntoOwned(frame.Take()));
        }
    }

    return ZX_OK;
}

zx_status_t Station::HandleDataFrame(DataFrame<>&& frame) {
    auto data_frame = frame.View();
    if (kFinspectEnabled) { DumpDataFrame(data_frame); }

    WLAN_STATS_INC(data_frame.in);
    if (ShouldDropDataFrame(data_frame)) { return ZX_ERR_NOT_SUPPORTED; }

    auto rssi_dbm = frame.View().rx_info()->rssi_dbm;
    WLAN_RSSI_HIST_INC(assoc_data_rssi, rssi_dbm);

    if (auto amsdu_frame = data_frame.CheckBodyType<AmsduSubframeHeader>().CheckLength()) {
        HandleAmsduFrame(amsdu_frame.IntoOwned(frame.Take()));
    } else if (auto llc_frame = data_frame.CheckBodyType<LlcHeader>().CheckLength()) {
        HandleDataFrame(llc_frame.IntoOwned(frame.Take()));
    } else if (auto null_frame = data_frame.CheckBodyType<NullDataHdr>().CheckLength()) {
        HandleNullDataFrame(null_frame.IntoOwned(frame.Take()));
    }

    return ZX_OK;
}

zx_status_t Station::Authenticate(wlan_mlme::AuthenticationTypes auth_type, uint32_t timeout) {
    debugfn();
    WLAN_STATS_INC(svc_msg.in);

    if (state_ != WlanState::kIdle) {
        errorf("received AUTHENTICATE.request in unexpected state: %u\n", state_);
        return service::SendAuthConfirm(device_, join_ctx_->bssid(),
                                        wlan_mlme::AuthenticateResultCodes::REFUSED);
    }

    if (auth_type != wlan_mlme::AuthenticationTypes::OPEN_SYSTEM) {
        errorf("only OpenSystem authentication is supported\n");
        return service::SendAuthConfirm(device_, join_ctx_->bssid(),
                                        wlan_mlme::AuthenticateResultCodes::REFUSED);
    }

    debugjoin("authenticating to %s\n", join_ctx_->bssid().ToString().c_str());

    constexpr size_t max_frame_len = MgmtFrameHeader::max_len() + Authentication::max_len();
    auto packet = GetWlanPacket(max_frame_len);
    if (packet == nullptr) { return ZX_ERR_NO_RESOURCES; }

    BufferWriter w(*packet);
    auto mgmt_hdr = w.Write<MgmtFrameHeader>();
    mgmt_hdr->fc.set_type(FrameType::kManagement);
    mgmt_hdr->fc.set_subtype(ManagementSubtype::kAuthentication);
    mgmt_hdr->addr1 = join_ctx_->bssid();
    mgmt_hdr->addr2 = self_addr();
    mgmt_hdr->addr3 = join_ctx_->bssid();
    SetSeqNo(mgmt_hdr, &seq_);

    // This assumes Open System authentication.
    auto auth = w.Write<Authentication>();
    auth->auth_algorithm_number = auth_alg_;
    auth->auth_txn_seq_number = 1;
    auth->status_code = 0;  // Reserved: explicitly set to 0

    zx::time deadline = deadline_after_bcn_period(timeout);
    auto status = timer_mgr_.Schedule(deadline, {}, &auth_timeout_);
    if (status != ZX_OK) {
        errorf("could not set authentication timeout event: %s\n", zx_status_get_string(status));
        // This is the wrong result code, but we need to define our own codes at some later time.
        service::SendAuthConfirm(device_, join_ctx_->bssid(),
                                 wlan_mlme::AuthenticateResultCodes::REFUSED);
        return status;
    }

    packet->set_len(w.WrittenBytes());

    finspect("Outbound Mgmt Frame(Auth): %s\n", debug::Describe(*mgmt_hdr).c_str());
    status = SendMgmtFrame(std::move(packet));
    if (status != ZX_OK) {
        errorf("could not send authentication frame: %d\n", status);
        service::SendAuthConfirm(device_, join_ctx_->bssid(),
                                 wlan_mlme::AuthenticateResultCodes::REFUSED);
        return status;
    }

    state_ = WlanState::kAuthenticating;
    return status;
}

zx_status_t Station::Deauthenticate(wlan_mlme::ReasonCode reason_code) {
    debugfn();
    WLAN_STATS_INC(svc_msg.in);

    if (state_ != WlanState::kAssociated && state_ != WlanState::kAuthenticated) {
        errorf("not associated or authenticated; ignoring deauthenticate request\n");
        return ZX_OK;
    }

    auto status = SendDeauthFrame(reason_code);
    if (status != ZX_OK) {
        errorf("could not send deauth packet: %d\n", status);
        // Deauthenticate nevertheless. IEEE isn't clear on what we are supposed to do.
    }
    infof("deauthenticating from \"%s\" (%s), reason=%hu\n",
          debug::ToAsciiOrHexStr(join_ctx_->bss()->ssid).c_str(),
          join_ctx_->bssid().ToString().c_str(), reason_code);

    if (state_ == WlanState::kAssociated) { device_->ClearAssoc(join_ctx_->bssid()); }
    state_ = WlanState::kIdle;
    device_->SetStatus(0);
    controlled_port_ = eapol::PortState::kBlocked;
    bu_queue_.clear();
    service::SendDeauthConfirm(device_, join_ctx_->bssid());

    return ZX_OK;
}

zx_status_t Station::Associate(Span<const uint8_t> rsne) {
    debugfn();
    WLAN_STATS_INC(svc_msg.in);

    if (state_ != WlanState::kAuthenticated) {
        if (state_ == WlanState::kAssociated) {
            warnf("already associated; sending request anyway\n");
        } else {
            // TODO(tkilbourn): better result codes
            errorf("must authenticate before associating\n");
            return service::SendAuthConfirm(device_, join_ctx_->bssid(),
                                            wlan_mlme::AuthenticateResultCodes::REFUSED);
        }
    }

    debugjoin("associating to %s\n", join_ctx_->bssid().ToString().c_str());

    constexpr size_t reserved_ie_len = 128;
    constexpr size_t max_frame_len =
        MgmtFrameHeader::max_len() + AssociationRequest::max_len() + reserved_ie_len;
    auto packet = GetWlanPacket(max_frame_len);
    if (packet == nullptr) {
        service::SendAssocConfirm(device_, wlan_mlme::AssociateResultCodes::REFUSED_TEMPORARILY);
        return ZX_ERR_NO_RESOURCES;
    }

    BufferWriter w(*packet);
    auto mgmt_hdr = w.Write<MgmtFrameHeader>();
    mgmt_hdr->fc.set_type(FrameType::kManagement);
    mgmt_hdr->fc.set_subtype(ManagementSubtype::kAssociationRequest);
    mgmt_hdr->addr1 = join_ctx_->bssid();
    mgmt_hdr->addr2 = self_addr();
    mgmt_hdr->addr3 = join_ctx_->bssid();
    SetSeqNo(mgmt_hdr, &seq_);

    auto ifc_info = device_->GetWlanInfo().ifc_info;
    auto client_capability = MakeClientAssocCtx(ifc_info, join_ctx_->channel());
    auto assoc = w.Write<AssociationRequest>();
    assoc->cap = OverrideCapability(client_capability.cap);
    assoc->listen_interval = 0;
    join_ctx_->set_listen_interval(assoc->listen_interval);

    auto rates = BuildAssocReqSuppRates(join_ctx_->bss()->basic_rate_set,
                                        join_ctx_->bss()->op_rate_set, client_capability.rates);
    if (!rates.has_value()) {
        service::SendAssocConfirm(device_,
                                  wlan_mlme::AssociateResultCodes::REFUSED_BASIC_RATES_MISMATCH);
        return ZX_ERR_NOT_SUPPORTED;
    } else if (rates->empty()) {
        service::SendAssocConfirm(device_,
                                  wlan_mlme::AssociateResultCodes::REFUSED_CAPABILITIES_MISMATCH);
        return ZX_ERR_NOT_SUPPORTED;
    }

    BufferWriter elem_w(w.RemainingBuffer());
    common::WriteSsid(&elem_w, join_ctx_->bss()->ssid);
    RatesWriter rates_writer{*rates};
    rates_writer.WriteSupportedRates(&elem_w);
    rates_writer.WriteExtendedSupportedRates(&elem_w);

    // Write RSNE from MLME-Association.request if available.
    if (!rsne.empty()) { elem_w.Write(rsne); }

    if (join_ctx_->IsHt() || join_ctx_->IsVht()) {
        auto ht_cap = client_capability.ht_cap.value_or(HtCapabilities{});
        debugf("HT cap(hardware reports): %s\n", debug::Describe(ht_cap).c_str());

        zx_status_t status = OverrideHtCapability(&ht_cap);
        if (status != ZX_OK) {
            errorf("could not build HtCapabilities. status %d\n", status);
            service::SendAssocConfirm(device_,
                                      wlan_mlme::AssociateResultCodes::REFUSED_REASON_UNSPECIFIED);
            return ZX_ERR_IO;
        }
        debugf("HT cap(after overriding): %s\n", debug::Describe(ht_cap).c_str());

        common::WriteHtCapabilities(&elem_w, ht_cap);
    }

    if (join_ctx_->IsVht()) {
        auto vht_cap = client_capability.vht_cap.value_or(VhtCapabilities{});
        // debugf("VHT cap(hardware reports): %s\n", debug::Describe(vht_cap).c_str());
        if (auto status = OverrideVhtCapability(&vht_cap, *join_ctx_); status != ZX_OK) {
            errorf("could not build VhtCapabilities (%s)\n", zx_status_get_string(status));
            service::SendAssocConfirm(device_,
                                      wlan_mlme::AssociateResultCodes::REFUSED_REASON_UNSPECIFIED);
            return ZX_ERR_IO;
        }
        // debugf("VHT cap(after overriding): %s\n", debug::Describe(vht_cap).c_str());
        common::WriteVhtCapabilities(&elem_w, vht_cap);
    }

    packet->set_len(w.WrittenBytes() + elem_w.WrittenBytes());

    finspect("Outbound Mgmt Frame (AssocReq): %s\n", debug::Describe(*mgmt_hdr).c_str());
    zx_status_t status = SendMgmtFrame(std::move(packet));
    if (status != ZX_OK) {
        errorf("could not send assoc packet: %d\n", status);
        service::SendAssocConfirm(device_,
                                  wlan_mlme::AssociateResultCodes::REFUSED_REASON_UNSPECIFIED);
        return status;
    }

    // TODO(NET-500): Add association timeout to MLME-ASSOCIATE.request just like
    // JOIN and AUTHENTICATE requests do.
    zx::time deadline = deadline_after_bcn_period(kAssocBcnCountTimeout);
    status = timer_mgr_.Schedule(deadline, {}, &assoc_timeout_);
    if (status != ZX_OK) {
        errorf("could not set auth timedout event: %d\n", status);
        // This is the wrong result code, but we need to define our own codes at some later time.
        service::SendAssocConfirm(device_,
                                  wlan_mlme::AssociateResultCodes::REFUSED_REASON_UNSPECIFIED);
        // TODO(tkilbourn): reset the station?
    }
    return status;
}

bool Station::ShouldDropMgmtFrame(const MgmtFrameView<>& frame) {
    // Drop management frames if either, there is no BSSID set yet,
    // or the frame is not from the BSS.
    return join_ctx_->bssid() != frame.hdr()->addr3;
}

// TODO(NET-500): Using a single method for joining and associated state is not ideal.
// The logic should be split up and decided on a higher level based on the current state.
void Station::HandleBeacon(MgmtFrame<Beacon>&& frame) {
    debugfn();

    auto rssi_dbm = frame.View().rx_info()->rssi_dbm;
    avg_rssi_dbm_.add(dBm(rssi_dbm));
    WLAN_RSSI_HIST_INC(beacon_rssi, rssi_dbm);

    if (state_ != WlanState::kAssociated) { return; }

    remaining_auto_deauth_timeout_ = FullAutoDeauthDuration();
    auto_deauth_last_accounted_ = timer_mgr_.Now();

    auto bcn_frame = frame.View().NextFrame();
    Span<const uint8_t> ie_chain = bcn_frame.body_data();
    auto tim = common::FindAndParseTim(ie_chain);
    if (tim && common::IsTrafficBuffered(assoc_ctx_.aid, tim->header, tim->bitmap)) {
        SendPsPoll();
    }
}

zx_status_t Station::HandleAuthentication(MgmtFrame<Authentication>&& frame) {
    debugfn();

    if (state_ != WlanState::kAuthenticating) {
        debugjoin("unexpected authentication frame in state: %u; ignoring frame\n", state_);
        return ZX_OK;
    }

    // Authentication notification received. Cancel pending timeout.
    timer_mgr_.Cancel(auth_timeout_);

    auto auth_hdr = frame.body_data();
    zx_status_t status = rust_mlme_is_valid_open_auth_resp(auth_hdr.data(), auth_hdr.size());
    if (status == ZX_OK) {
        state_ = WlanState::kAuthenticated;
        debugjoin("authenticated to %s\n", join_ctx_->bssid().ToString().c_str());
        service::SendAuthConfirm(device_, join_ctx_->bssid(),
                                 wlan_mlme::AuthenticateResultCodes::SUCCESS);
    } else {
        state_ = WlanState::kIdle;
        service::SendAuthConfirm(device_, join_ctx_->bssid(),
                                 wlan_mlme::AuthenticateResultCodes::AUTHENTICATION_REJECTED);
    }
    return status;
}

zx_status_t Station::HandleDeauthentication(MgmtFrame<Deauthentication>&& frame) {
    debugfn();

    if (state_ != WlanState::kAssociated && state_ != WlanState::kAuthenticated) {
        debugjoin("got spurious deauthenticate; ignoring\n");
        return ZX_OK;
    }

    auto deauth = frame.body();
    infof("deauthenticating from \"%s\" (%s), reason=%hu\n",
          debug::ToAsciiOrHexStr(join_ctx_->bss()->ssid).c_str(),
          join_ctx_->bssid().ToString().c_str(), deauth->reason_code);

    if (state_ == WlanState::kAssociated) { device_->ClearAssoc(join_ctx_->bssid()); }
    state_ = WlanState::kIdle;
    device_->SetStatus(0);
    controlled_port_ = eapol::PortState::kBlocked;
    bu_queue_.clear();

    return service::SendDeauthIndication(device_, join_ctx_->bssid(),
                                         static_cast<wlan_mlme::ReasonCode>(deauth->reason_code));
}

zx_status_t Station::HandleAssociationResponse(MgmtFrame<AssociationResponse>&& frame) {
    debugfn();

    if (state_ != WlanState::kAuthenticated) {
        // TODO(tkilbourn): should we process this Association response packet anyway? The spec is
        // unclear.
        debugjoin("unexpected association response frame\n");
        return ZX_OK;
    }

    // Receive association response, cancel association timeout.
    timer_mgr_.Cancel(assoc_timeout_);

    auto assoc = frame.body();
    if (assoc->status_code != status_code::kSuccess) {
        errorf("association failed (status code=%u)\n", assoc->status_code);
        // TODO(tkilbourn): map to the correct result code
        service::SendAssocConfirm(device_,
                                  wlan_mlme::AssociateResultCodes::REFUSED_REASON_UNSPECIFIED);
        return ZX_ERR_BAD_STATE;
    }

    auto status = SetAssocContext(frame.View());
    if (status != ZX_OK) {
        errorf("failed to set association context (status %d)\n", status);
        service::SendAssocConfirm(device_,
                                  wlan_mlme::AssociateResultCodes::REFUSED_REASON_UNSPECIFIED);
        return ZX_ERR_BAD_STATE;
    }

    // TODO(porce): Move into |assoc_ctx_|
    state_ = WlanState::kAssociated;
    assoc_ctx_.aid = assoc->aid;

    // Spread the good news upward
    service::SendAssocConfirm(device_, wlan_mlme::AssociateResultCodes::SUCCESS, assoc_ctx_.aid);
    // Spread the good news downward
    NotifyAssocContext();

    // Initiate RSSI reporting to Wlanstack.
    zx::time deadline = deadline_after_bcn_period(kSignalReportBcnCountTimeout);
    timer_mgr_.Schedule(deadline, {}, &signal_report_timeout_);
    avg_rssi_dbm_.reset();
    avg_rssi_dbm_.add(dBm(frame.View().rx_info()->rssi_dbm));
    service::SendSignalReportIndication(device_, common::dBm(frame.View().rx_info()->rssi_dbm));

    remaining_auto_deauth_timeout_ = FullAutoDeauthDuration();
    status = timer_mgr_.Schedule(timer_mgr_.Now() + remaining_auto_deauth_timeout_, {},
                                 &auto_deauth_timeout_);
    if (status != ZX_OK) { warnf("could not set auto-deauthentication timeout event\n"); }

    // Open port if user connected to an open network.
    if (join_ctx_->bss()->rsn.is_null()) {
        debugjoin("802.1X controlled port is now open\n");
        controlled_port_ = eapol::PortState::kOpen;
        device_->SetStatus(ETHMAC_STATUS_ONLINE);
    }

    infof("NIC %s associated with \"%s\"(%s) in channel %s, %s, %s\n",
          self_addr().ToString().c_str(), debug::ToAsciiOrHexStr(join_ctx_->bss()->ssid).c_str(),
          assoc_ctx_.bssid.ToString().c_str(), common::ChanStrLong(assoc_ctx_.chan).c_str(),
          common::BandStr(assoc_ctx_.chan).c_str(), common::GetPhyStr(assoc_ctx_.phy).c_str());

    // TODO(porce): Time when to establish BlockAck session
    // Handle MLME-level retry, if MAC-level retry ultimately fails
    // Wrap this as EstablishBlockAckSession(peer_mac_addr)
    // Signal to lower MAC for proper session handling

    if (join_ctx_->IsHt() || join_ctx_->IsVht()) { SendAddBaRequestFrame(); }
    return ZX_OK;
}

zx_status_t Station::HandleDisassociation(MgmtFrame<Disassociation>&& frame) {
    debugfn();

    if (state_ != WlanState::kAssociated) {
        debugjoin("got spurious disassociate; ignoring\n");
        return ZX_OK;
    }

    auto disassoc = frame.body();
    infof("disassociating from \"%s\"(%s), reason=%u\n",
          debug::ToAsciiOrHexStr(join_ctx_->bss()->ssid).c_str(),
          join_ctx_->bssid().ToString().c_str(), disassoc->reason_code);

    state_ = WlanState::kAuthenticated;
    device_->ClearAssoc(join_ctx_->bssid());
    device_->SetStatus(0);
    controlled_port_ = eapol::PortState::kBlocked;
    timer_mgr_.Cancel(signal_report_timeout_);
    bu_queue_.clear();

    return service::SendDisassociateIndication(device_, join_ctx_->bssid(), disassoc->reason_code);
}

zx_status_t Station::HandleActionFrame(MgmtFrame<ActionFrame>&& frame) {
    debugfn();

    auto action_frame = frame.View().NextFrame();
    if (auto action_ba_frame = action_frame.CheckBodyType<ActionFrameBlockAck>().CheckLength()) {
        auto ba_frame = action_ba_frame.NextFrame();
        if (auto add_ba_resp_frame = ba_frame.CheckBodyType<AddBaResponseFrame>().CheckLength()) {
            finspect("Inbound ADDBA Resp frame: len %zu\n", add_ba_resp_frame.body_len());
            finspect("  addba resp: %s\n", debug::Describe(*add_ba_resp_frame.body()).c_str());

            // TODO(porce): Handle AddBaResponses and keep the result of negotiation.
        } else if (auto add_ba_req_frame =
                       ba_frame.CheckBodyType<AddBaRequestFrame>().CheckLength()) {
            finspect("Inbound ADDBA Req frame: len %zu\n", add_ba_req_frame.body_len());
            finspect("  addba req: %s\n", debug::Describe(*add_ba_req_frame.body()).c_str());

            return HandleAddBaRequest(*add_ba_req_frame.body());
        }
    }

    return ZX_OK;
}

zx_status_t Station::HandleAddBaRequest(const AddBaRequestFrame& addbareq) {
    debugfn();

    constexpr size_t max_frame_len = MgmtFrameHeader::max_len() + ActionFrame::max_len() +
                                     ActionFrameBlockAck::max_len() + AddBaRequestFrame::max_len();
    auto packet = GetWlanPacket(max_frame_len);
    if (packet == nullptr) { return ZX_ERR_NO_RESOURCES; }

    BufferWriter w(*packet);
    auto mgmt_hdr = w.Write<MgmtFrameHeader>();
    mgmt_hdr->fc.set_type(FrameType::kManagement);
    mgmt_hdr->fc.set_subtype(ManagementSubtype::kAction);
    mgmt_hdr->addr1 = join_ctx_->bssid();
    mgmt_hdr->addr2 = self_addr();
    mgmt_hdr->addr3 = join_ctx_->bssid();
    SetSeqNo(mgmt_hdr, &seq_);

    w.Write<ActionFrame>()->category = ActionFrameBlockAck::ActionCategory();
    w.Write<ActionFrameBlockAck>()->action = AddBaResponseFrame::BlockAckAction();

    auto addbaresp_hdr = w.Write<AddBaResponseFrame>();
    addbaresp_hdr->dialog_token = addbareq.dialog_token;

    // TODO(porce): Implement DelBa as a response to AddBar for decline

    // Note: Returning AddBaResponse with status_code::kRefused seems ineffective.
    // ArubaAP is persistent not honoring that.
    addbaresp_hdr->status_code = status_code::kSuccess;

    addbaresp_hdr->params.set_amsdu(addbareq.params.amsdu() == 1);
    addbaresp_hdr->params.set_policy(BlockAckParameters::kImmediate);
    addbaresp_hdr->params.set_tid(addbareq.params.tid());

    // TODO(NET-500): Is this Ralink specific?
    // TODO(porce): Once chipset capability is ready, refactor below buffer_size
    // calculation.
    size_t buffer_size_ap = addbareq.params.buffer_size();
    constexpr size_t buffer_size_ralink = 64;
    size_t buffer_size =
        (buffer_size_ap <= buffer_size_ralink) ? buffer_size_ap : buffer_size_ralink;
    addbaresp_hdr->params.set_buffer_size(buffer_size);
    addbaresp_hdr->timeout = addbareq.timeout;

    packet->set_len(w.WrittenBytes());

    finspect("Outbound ADDBA Resp frame: len %zu\n", w.WrittenBytes());
    finspect("Outbound Mgmt Frame(ADDBA Resp): %s\n", debug::Describe(*addbaresp_hdr).c_str());

    auto status = SendMgmtFrame(std::move(packet));
    if (status != ZX_OK) { errorf("could not send AddBaResponse: %d\n", status); }
    return status;
}

bool Station::ShouldDropDataFrame(const DataFrameView<>& frame) {
    if (state_ != WlanState::kAssociated) { return true; }

    return join_ctx_->bssid() != frame.hdr()->addr2;
}

zx_status_t Station::HandleNullDataFrame(DataFrame<NullDataHdr>&& frame) {
    debugfn();
    ZX_DEBUG_ASSERT(state_ == WlanState::kAssociated);

    // Take signal strength into account.
    avg_rssi_dbm_.add(dBm(frame.View().rx_info()->rssi_dbm));

    // Some AP's such as Netgear Routers send periodic NULL data frames to test whether a client
    // timed out. The client must respond with a NULL data frame itself to not get
    // deauthenticated.
    SendKeepAliveResponse();
    return ZX_OK;
}

zx_status_t Station::HandleDataFrame(DataFrame<LlcHeader>&& frame) {
    debugfn();
    ZX_DEBUG_ASSERT(state_ == WlanState::kAssociated);

    auto data_llc_frame = frame.View();
    auto data_hdr = data_llc_frame.hdr();

    // Take signal strength into account.
    avg_rssi_dbm_.add(dBm(frame.View().rx_info()->rssi_dbm));

    // Forward EAPOL frames to SME.
    auto llc_frame = data_llc_frame.SkipHeader();
    if (auto eapol_frame = llc_frame.CheckBodyType<EapolHdr>().CheckLength().SkipHeader()) {
        if (eapol_frame.body_len() == eapol_frame.hdr()->get_packet_body_length()) {
            return service::SendEapolIndication(device_, *eapol_frame.hdr(), data_hdr->addr3,
                                                data_hdr->addr1);
        } else {
            errorf("received invalid EAPOL frame\n");
        }
        return ZX_OK;
    }

    // Drop packets if RSNA was not yet established.
    if (controlled_port_ == eapol::PortState::kBlocked) { return ZX_OK; }

    // PS-POLL if there are more buffered unicast frames.
    if (data_hdr->fc.more_data() && data_hdr->addr1.IsUcast()) { SendPsPoll(); }

    const auto& src = data_hdr->addr3;
    const auto& dest = data_hdr->addr1;
    size_t llc_payload_len = llc_frame.body_len();
    return HandleLlcFrame(llc_frame, llc_payload_len, src, dest);
}

zx_status_t Station::HandleLlcFrame(const FrameView<LlcHeader>& llc_frame, size_t llc_payload_len,
                                    const common::MacAddr& src, const common::MacAddr& dest) {
    finspect("Inbound LLC frame: hdr len %zu, payload len: %zu\n", llc_frame.hdr()->len(),
             llc_payload_len);
    finspect("  llc hdr: %s\n", debug::Describe(*llc_frame.hdr()).c_str());
    finspect("  llc payload: %s\n",
             debug::HexDump(llc_frame.body_data().subspan(0, llc_payload_len)).c_str());
    if (llc_payload_len == 0) {
        finspect("  dropping empty LLC frame\n");
        return ZX_OK;
    }

    // Prepare a packet
    const size_t eth_frame_len = EthernetII::max_len() + llc_payload_len;
    auto packet = GetEthPacket(eth_frame_len);
    if (packet == nullptr) { return ZX_ERR_NO_RESOURCES; }

    BufferWriter w(*packet);
    auto eth_hdr = w.Write<EthernetII>();
    eth_hdr->dest = dest;
    eth_hdr->src = src;
    eth_hdr->ether_type = llc_frame.hdr()->protocol_id;
    w.Write(llc_frame.body_data().subspan(0, llc_payload_len));

    packet->set_len(w.WrittenBytes());

    auto status = device_->DeliverEthernet(*packet);
    if (status != ZX_OK) { errorf("could not send ethernet data: %d\n", status); }
    return status;
}

zx_status_t Station::HandleAmsduFrame(DataFrame<AmsduSubframeHeader>&& frame) {
    // TODO(porce): Define A-MSDU or MSDU signature, and avoid forceful conversion.
    debugfn();
    auto data_amsdu_frame = frame.View();

    // Non-DMG stations use basic subframe format only.
    if (data_amsdu_frame.body_len() == 0) { return ZX_OK; }
    finspect("Inbound AMSDU: len %zu\n", data_amsdu_frame.body_len());

    // TODO(porce): The received AMSDU should not be greater than max_amsdu_len, specified in
    // HtCapabilities IE of Association. Warn or discard if violated.

    const auto& src = data_amsdu_frame.hdr()->addr3;
    const auto& dest = data_amsdu_frame.hdr()->addr1;
    DeaggregateAmsdu(data_amsdu_frame, [&](FrameView<LlcHeader> llc_frame, size_t payload_len) {
        HandleLlcFrame(llc_frame, payload_len, src, dest);
    });

    return ZX_OK;
}

zx_status_t Station::HandleEthFrame(EthFrame&& eth_frame) {
    debugfn();
    if (state_ != WlanState::kAssociated) {
        debugf("dropping eth packet while not associated\n");
        return ZX_ERR_BAD_STATE;
    }

    // If off channel, buffer Ethernet frame
    if (!chan_sched_->OnChannel()) {
        if (bu_queue_.size() >= kMaxPowerSavingQueueSize) {
            bu_queue_.Dequeue();
            warnf("dropping oldest unicast frame\n");
        }
        bu_queue_.Enqueue(eth_frame.Take());
        debugps("queued frame since off channel; bu queue size: %lu\n", bu_queue_.size());
        return ZX_OK;
    }

    auto eth_hdr = eth_frame.hdr();
    const size_t frame_len =
        DataFrameHeader::max_len() + LlcHeader::max_len() + eth_frame.body_len();
    auto packet = GetWlanPacket(frame_len);
    if (packet == nullptr) { return ZX_ERR_NO_RESOURCES; }

    bool needs_protection =
        !join_ctx_->bss()->rsn.is_null() && controlled_port_ == eapol::PortState::kOpen;
    BufferWriter w(*packet);

    auto data_hdr = w.Write<DataFrameHeader>();
    bool has_ht_ctrl = false;
    data_hdr->fc.set_type(FrameType::kData);
    data_hdr->fc.set_subtype(IsQosReady() ? DataSubtype::kQosdata : DataSubtype::kDataSubtype);
    data_hdr->fc.set_to_ds(1);
    data_hdr->fc.set_from_ds(0);
    data_hdr->fc.set_htc_order(has_ht_ctrl ? 1 : 0);
    data_hdr->fc.set_protected_frame(needs_protection);
    data_hdr->addr1 = join_ctx_->bssid();
    data_hdr->addr2 = eth_hdr->src;
    data_hdr->addr3 = eth_hdr->dest;
    SetSeqNo(data_hdr, &seq_);

    // TODO(porce): Construct addr4 field

    if (IsQosReady()) {  // QoS Control field
        auto qos_ctrl = w.Write<QosControl>();
        qos_ctrl->set_tid(GetTid(eth_frame));
        qos_ctrl->set_eosp(0);
        qos_ctrl->set_ack_policy(ack_policy::kNormalAck);

        // AMSDU: set_amsdu_present(1) requires dot11HighthroughputOptionImplemented should be true.
        qos_ctrl->set_amsdu_present(0);
        qos_ctrl->set_byte(0);
    }

    // TODO(porce): Construct htc_order field

    auto llc_hdr = w.Write<LlcHeader>();
    FillEtherLlcHeader(llc_hdr, eth_hdr->ether_type);
    w.Write(eth_frame.body_data());

    packet->set_len(w.WrittenBytes());

    finspect("Outbound data frame: len %zu\n", w.WrittenBytes());
    finspect("  wlan hdr: %s\n", debug::Describe(*data_hdr).c_str());
    finspect("  llc  hdr: %s\n", debug::Describe(*llc_hdr).c_str());
    finspect("  frame   : %s\n", debug::HexDump(packet->data(), packet->len()).c_str());

    auto status = SendDataFrame(std::move(packet), data_hdr->addr3.IsUcast());
    if (status != ZX_OK) { errorf("could not send wlan data: %d\n", status); }
    return status;
}

zx_status_t Station::HandleTimeout() {
    debugfn();

    zx_status_t status = timer_mgr_.HandleTimeout([&](auto now, auto _event, auto timeout_id) {
        if (timeout_id == auth_timeout_) {
            debugjoin("auth timed out; moving back to idle state\n");
            state_ = WlanState::kIdle;
            service::SendAuthConfirm(device_, join_ctx_->bssid(),
                                     wlan_mlme::AuthenticateResultCodes::AUTH_FAILURE_TIMEOUT);
        } else if (timeout_id == assoc_timeout_) {
            debugjoin("assoc timed out; moving back to authenticated\n");
            // TODO(tkilbourn): need a better error code for this
            service::SendAssocConfirm(device_,
                                      wlan_mlme::AssociateResultCodes::REFUSED_TEMPORARILY);
        } else if (timeout_id == signal_report_timeout_) {
            if (state_ == WlanState::kAssociated) {
                service::SendSignalReportIndication(device_, common::to_dBm(avg_rssi_dbm_.avg()));

                zx::time deadline = deadline_after_bcn_period(kSignalReportBcnCountTimeout);
                timer_mgr_.Schedule(deadline, {}, &signal_report_timeout_);
            }
        } else if (timeout_id == auto_deauth_timeout_) {
            debugclt("now: %lu\n", now.get());
            debugclt("remaining auto-deauth timeout: %lu\n", remaining_auto_deauth_timeout_.get());
            debugclt("auto-deauth last accounted time: %lu\n", auto_deauth_last_accounted_.get());

            if (!chan_sched_->OnChannel()) {
                ZX_DEBUG_ASSERT("auto-deauth timeout should not trigger while off channel\n");
            } else if (remaining_auto_deauth_timeout_ > now - auto_deauth_last_accounted_) {
                // Update the remaining auto-deauth timeout with the unaccounted time
                remaining_auto_deauth_timeout_ -= now - auto_deauth_last_accounted_;
                auto_deauth_last_accounted_ = now;
                timer_mgr_.Schedule(now + remaining_auto_deauth_timeout_, {},
                                    &auto_deauth_timeout_);
            } else if (state_ == WlanState::kAssociated) {
                infof("lost BSS; deauthenticating...\n");
                state_ = WlanState::kIdle;
                device_->ClearAssoc(join_ctx_->bssid());
                device_->SetStatus(0);
                controlled_port_ = eapol::PortState::kBlocked;

                auto reason_code = wlan_mlme::ReasonCode::LEAVING_NETWORK_DEAUTH;
                service::SendDeauthIndication(device_, join_ctx_->bssid(), reason_code);
                auto status = SendDeauthFrame(reason_code);
                if (status != ZX_OK) { errorf("could not send deauth packet: %d\n", status); }
            }
        }
    });

    if (status != ZX_OK) {
        errorf("failed to rearm the timer after handling the timeout: %s",
               zx_status_get_string(status));
    }

    return status;
}

zx_status_t Station::SendKeepAliveResponse() {
    if (state_ != WlanState::kAssociated) {
        warnf("cannot send keep alive response before being associated\n");
        return ZX_OK;
    }

    auto packet = GetWlanPacket(DataFrameHeader::max_len());
    if (packet == nullptr) { return ZX_ERR_NO_RESOURCES; }

    BufferWriter w(*packet);
    auto data_hdr = w.Write<DataFrameHeader>();
    data_hdr->fc.set_type(FrameType::kData);
    data_hdr->fc.set_subtype(DataSubtype::kNull);
    data_hdr->fc.set_to_ds(1);
    data_hdr->addr1 = join_ctx_->bssid();
    data_hdr->addr2 = self_addr();
    data_hdr->addr3 = join_ctx_->bssid();
    SetSeqNo(data_hdr, &seq_);

    packet->set_len(w.WrittenBytes());

    auto status = SendDataFrame(std::move(packet), true);
    if (status != ZX_OK) {
        errorf("could not send keep alive frame: %d\n", status);
        return status;
    }
    return ZX_OK;
}

zx_status_t Station::SendAddBaRequestFrame() {
    debugfn();

    if (state_ != WlanState::kAssociated) {
        errorf("won't send ADDBA Request in other than Associated state. Current state: %d\n",
               state_);
        return ZX_ERR_BAD_STATE;
    }

    constexpr size_t max_frame_size = MgmtFrameHeader::max_len() + ActionFrame::max_len() +
                                      ActionFrameBlockAck::max_len() + AddBaRequestFrame::max_len();
    auto packet = GetWlanPacket(max_frame_size);
    if (packet == nullptr) { return ZX_ERR_NO_RESOURCES; }

    BufferWriter w(*packet);
    auto mgmt_hdr = w.Write<MgmtFrameHeader>();
    mgmt_hdr->fc.set_type(FrameType::kManagement);
    mgmt_hdr->fc.set_subtype(ManagementSubtype::kAction);
    mgmt_hdr->addr1 = join_ctx_->bssid();
    mgmt_hdr->addr2 = self_addr();
    mgmt_hdr->addr3 = join_ctx_->bssid();
    SetSeqNo(mgmt_hdr, &seq_);

    auto action_hdr = w.Write<ActionFrame>();
    action_hdr->category = ActionFrameBlockAck::ActionCategory();

    auto ba_hdr = w.Write<ActionFrameBlockAck>();
    ba_hdr->action = AddBaRequestFrame::BlockAckAction();

    auto addbareq_hdr = w.Write<AddBaRequestFrame>();
    // It appears there is no particular rule to choose the value for
    // dialog_token. See IEEE Std 802.11-2016, 9.6.5.2.
    addbareq_hdr->dialog_token = 0x01;
    addbareq_hdr->params.set_amsdu(1);
    addbareq_hdr->params.set_policy(BlockAckParameters::BlockAckPolicy::kImmediate);
    addbareq_hdr->params.set_tid(GetTid());  // TODO(porce): Communicate this with lower MAC.
    // TODO(porce): Fix the discrepancy of this value from the Ralink's TXWI ba_win_size setting
    addbareq_hdr->params.set_buffer_size(64);
    addbareq_hdr->timeout = 0;               // Disables the timeout
    addbareq_hdr->seq_ctrl.set_fragment(0);  // TODO(porce): Send this down to the lower MAC
    addbareq_hdr->seq_ctrl.set_starting_seq(1);

    packet->set_len(w.WrittenBytes());

    finspect("Outbound ADDBA Req frame: len %zu\n", w.WrittenBytes());
    finspect("  addba req: %s\n", debug::Describe(*addbareq_hdr).c_str());

    auto status = SendMgmtFrame(std::move(packet));
    if (status != ZX_OK) {
        errorf("could not send AddBaRequest: %d\n", status);
        return status;
    }

    return ZX_OK;
}

zx_status_t Station::SendEapolFrame(Span<const uint8_t> eapol_frame, const common::MacAddr& src,
                                    const common::MacAddr& dst) {
    debugfn();
    WLAN_STATS_INC(svc_msg.in);

    if (state_ != WlanState::kAssociated) {
        debugf("dropping MLME-EAPOL.request while not being associated. STA in state %d\n", state_);
        return ZX_OK;
    }

    const size_t llc_payload_len = eapol_frame.size_bytes();
    const size_t max_frame_len =
        DataFrameHeader::max_len() + LlcHeader::max_len() + llc_payload_len;
    auto packet = GetWlanPacket(max_frame_len);
    if (packet == nullptr) { return ZX_ERR_NO_RESOURCES; }

    bool needs_protection =
        !join_ctx_->bss()->rsn.is_null() && controlled_port_ == eapol::PortState::kOpen;
    BufferWriter w(*packet);

    auto data_hdr = w.Write<DataFrameHeader>();
    data_hdr->fc.set_type(FrameType::kData);
    data_hdr->fc.set_to_ds(1);
    data_hdr->fc.set_protected_frame(needs_protection);
    data_hdr->addr1 = dst;
    data_hdr->addr2 = src;
    data_hdr->addr3 = dst;
    SetSeqNo(data_hdr, &seq_);

    auto llc_hdr = w.Write<LlcHeader>();
    llc_hdr->dsap = kLlcSnapExtension;
    llc_hdr->ssap = kLlcSnapExtension;
    llc_hdr->control = kLlcUnnumberedInformation;
    std::memcpy(llc_hdr->oui, kLlcOui, sizeof(llc_hdr->oui));
    llc_hdr->protocol_id = htobe16(kEapolProtocolId);
    w.Write(eapol_frame);

    packet->set_len(w.WrittenBytes());

    zx_status_t status =
        SendDataFrame(std::move(packet), true, WLAN_TX_INFO_FLAGS_FAVOR_RELIABILITY);
    if (status != ZX_OK) {
        errorf("could not send eapol request packet: %d\n", status);
        service::SendEapolConfirm(device_, wlan_mlme::EapolResultCodes::TRANSMISSION_FAILURE);
        return status;
    }

    service::SendEapolConfirm(device_, wlan_mlme::EapolResultCodes::SUCCESS);

    return status;
}

zx_status_t Station::SetKeys(Span<const wlan_mlme::SetKeyDescriptor> keys) {
    debugfn();
    WLAN_STATS_INC(svc_msg.in);

    for (auto& keyDesc : keys) {
        auto key_config = ToKeyConfig(keyDesc);
        if (!key_config.has_value()) { return ZX_ERR_NOT_SUPPORTED; }

        auto status = device_->SetKey(&key_config.value());
        if (status != ZX_OK) {
            errorf("Could not configure keys in hardware: %d\n", status);
            return status;
        }
    }

    return ZX_OK;
}

void Station::UpdateControlledPort(wlan_mlme::ControlledPortState state) {
    WLAN_STATS_INC(svc_msg.in);

    if (state == wlan_mlme::ControlledPortState::OPEN) {
        controlled_port_ = eapol::PortState::kOpen;
        device_->SetStatus(ETHMAC_STATUS_ONLINE);
    } else {
        controlled_port_ = eapol::PortState::kBlocked;
        device_->SetStatus(0);
    }
}

void Station::PreSwitchOffChannel() {
    debugfn();
    if (state_ == WlanState::kAssociated) {
        SetPowerManagementMode(true);

        timer_mgr_.Cancel(auto_deauth_timeout_);
        zx::duration unaccounted_time = timer_mgr_.Now() - auto_deauth_last_accounted_;
        if (remaining_auto_deauth_timeout_ > unaccounted_time) {
            remaining_auto_deauth_timeout_ -= unaccounted_time;
        } else {
            remaining_auto_deauth_timeout_ = zx::duration(0);
        }
    }
}

void Station::BackToMainChannel() {
    debugfn();
    if (state_ == WlanState::kAssociated) {
        SetPowerManagementMode(false);

        zx::time now = timer_mgr_.Now();
        auto deadline = now + std::max(remaining_auto_deauth_timeout_, WLAN_TU(1u));
        timer_mgr_.Schedule(deadline, {}, &auto_deauth_timeout_);
        auto_deauth_last_accounted_ = now;

        SendBufferedUnits();
    }
}

void Station::SendBufferedUnits() {
    while (bu_queue_.size() > 0) {
        fbl::unique_ptr<Packet> packet = bu_queue_.Dequeue();
        debugps("sending buffered frame; queue size at: %lu\n", bu_queue_.size());
        ZX_DEBUG_ASSERT(packet->peer() == Packet::Peer::kEthernet);
        HandleEthFrame(EthFrame(std::move(packet)));
    }
}

void Station::DumpDataFrame(const DataFrameView<>& frame) {
    // TODO(porce): Should change the API signature to MSDU
    auto hdr = frame.hdr();

    bool is_ucast_to_self = self_addr() == hdr->addr1;
    bool is_mcast = hdr->addr1.IsBcast();
    bool is_bcast = hdr->addr1.IsMcast();
    bool is_interesting = is_ucast_to_self || is_mcast || is_bcast;
    if (!is_interesting) { return; }

    bool from_bss = (join_ctx_->bssid() == hdr->addr2);
    if (state_ == WlanState::kAssociated && !from_bss) { return; }

    finspect("Inbound data frame: len %zu\n", frame.len());
    finspect("  wlan hdr: %s\n", debug::Describe(*hdr).c_str());
    finspect("  msdu    : %s\n", debug::HexDump(frame.body_data()).c_str());
}

zx_status_t Station::SendCtrlFrame(fbl::unique_ptr<Packet> packet, CBW cbw, PHY phy) {
    chan_sched_->EnsureOnChannel(timer_mgr_.Now() + kOnChannelTimeAfterSend);
    return SendWlan(std::move(packet), cbw, phy);
}

zx_status_t Station::SendMgmtFrame(fbl::unique_ptr<Packet> packet) {
    chan_sched_->EnsureOnChannel(timer_mgr_.Now() + kOnChannelTimeAfterSend);
    return SendWlan(std::move(packet), CBW20, WLAN_PHY_OFDM);
}

zx_status_t Station::SendDataFrame(fbl::unique_ptr<Packet> packet, bool unicast, uint32_t flags) {
    CBW cbw = CBW20;
    PHY phy = WLAN_PHY_OFDM;
    if (assoc_ctx_.phy == WLAN_PHY_HT) {
        if (assoc_ctx_.is_cbw40_tx && unicast) {
            // 40 MHz direction does not matter here.
            // Radio uses the operational channel setting. This indicates the bandwidth without
            // direction.
            cbw = CBW40;
        }
        phy = WLAN_PHY_HT;
    }

    return SendWlan(std::move(packet), cbw, phy, flags);
}

zx_status_t Station::SetPowerManagementMode(bool ps_mode) {
    if (state_ != WlanState::kAssociated) {
        warnf("cannot adjust power management before being associated\n");
        return ZX_OK;
    }

    auto packet = GetWlanPacket(DataFrameHeader::max_len());
    if (packet == nullptr) { return ZX_ERR_NO_RESOURCES; }

    BufferWriter w(*packet);
    auto data_hdr = w.Write<DataFrameHeader>();
    data_hdr->fc.set_type(FrameType::kData);
    data_hdr->fc.set_subtype(DataSubtype::kNull);
    data_hdr->fc.set_pwr_mgmt(ps_mode);
    data_hdr->fc.set_to_ds(1);
    data_hdr->addr1 = join_ctx_->bssid();
    data_hdr->addr2 = self_addr();
    data_hdr->addr3 = join_ctx_->bssid();
    SetSeqNo(data_hdr, &seq_);

    packet->set_len(w.WrittenBytes());
    auto status = SendDataFrame(std::move(packet), true);
    if (status != ZX_OK) {
        errorf("could not send power management frame: %d\n", status);
        return status;
    }
    return ZX_OK;
}

zx_status_t Station::SendPsPoll() {
    // TODO(hahnr): We should probably wait for an RSNA if the network is an
    // RSN. Else we cannot work with the incoming data frame.
    if (state_ != WlanState::kAssociated) {
        warnf("cannot send ps-poll before being associated\n");
        return ZX_OK;
    }

    constexpr size_t len = CtrlFrameHdr::max_len() + PsPollFrame::max_len();
    auto packet = GetWlanPacket(len);
    if (packet == nullptr) { return ZX_ERR_NO_RESOURCES; }

    BufferWriter w(*packet);
    auto fc = w.Write<FrameControl>();
    fc->set_type(FrameType::kControl);
    fc->set_subtype(ControlSubtype::kPsPoll);

    auto ps_poll = w.Write<PsPollFrame>();
    ps_poll->aid = assoc_ctx_.aid;
    ps_poll->bssid = join_ctx_->bssid();
    ps_poll->ta = self_addr();

    CBW cbw = (assoc_ctx_.is_cbw40_tx ? CBW40 : CBW20);

    packet->set_len(w.WrittenBytes());
    auto status = SendCtrlFrame(std::move(packet), cbw, WLAN_PHY_HT);
    if (status != ZX_OK) {
        errorf("could not send power management packet: %d\n", status);
        return status;
    }
    return ZX_OK;
}

zx_status_t Station::SendDeauthFrame(wlan_mlme::ReasonCode reason_code) {
    debugfn();

    constexpr size_t max_frame_len = MgmtFrameHeader::max_len() + Deauthentication::max_len();
    auto packet = GetWlanPacket(max_frame_len);
    if (packet == nullptr) { return ZX_ERR_NO_RESOURCES; }

    BufferWriter w(*packet);
    auto mgmt_hdr = w.Write<MgmtFrameHeader>();
    mgmt_hdr->fc.set_type(FrameType::kManagement);
    mgmt_hdr->fc.set_subtype(ManagementSubtype::kDeauthentication);
    mgmt_hdr->addr1 = join_ctx_->bssid();
    mgmt_hdr->addr2 = self_addr();
    mgmt_hdr->addr3 = join_ctx_->bssid();
    SetSeqNo(mgmt_hdr, &seq_);

    auto deauth = w.Write<Deauthentication>();
    deauth->reason_code = static_cast<uint16_t>(reason_code);

    finspect("Outbound Mgmt Frame(Deauth): %s\n", debug::Describe(*mgmt_hdr).c_str());
    packet->set_len(w.WrittenBytes());
    return SendMgmtFrame(std::move(packet));
}

zx_status_t Station::SendWlan(fbl::unique_ptr<Packet> packet, CBW cbw, PHY phy, uint32_t flags) {
    auto packet_bytes = packet->len();
    zx_status_t status = device_->SendWlan(std::move(packet), cbw, phy, flags);
    if (status == ZX_OK) {
        WLAN_STATS_INC(tx_frame.out);
        WLAN_STATS_ADD(packet_bytes, tx_frame.out_bytes);
    }
    return status;
}

zx::time Station::deadline_after_bcn_period(size_t bcn_count) {
    return timer_mgr_.Now() + WLAN_TU(join_ctx_->bss()->beacon_period * bcn_count);
}

zx::duration Station::FullAutoDeauthDuration() {
    return WLAN_TU(join_ctx_->bss()->beacon_period * kAutoDeauthBcnCountTimeout);
}

bool Station::IsCbw40Rx() const {
    // Station can receive CBW40 data frames only when
    // the AP is capable of transmitting CBW40,
    // the client is capable of receiving CBW40,
    // and the association is to configured to use CBW40.

    const auto& join_chan = join_ctx_->channel();
    auto ifc_info = device_->GetWlanInfo().ifc_info;
    auto client_assoc = MakeClientAssocCtx(ifc_info, join_chan);

    debugf(
        "IsCbw40Rx: join_chan.cbw:%u, bss.ht_cap:%s, bss.chan_width_set:%s "
        "client_assoc.has_ht_cap:%s "
        "client_assoc.chan_width_set:%u\n",
        join_chan.cbw, (join_ctx_->bss()->ht_cap != nullptr) ? "yes" : "no",
        (join_ctx_->bss()->ht_cap == nullptr)
            ? "invalid"
            : (join_ctx_->bss()->ht_cap->ht_cap_info.chan_width_set ==
               to_enum_type(wlan_mlme::ChanWidthSet::TWENTY_ONLY))
                  ? "20"
                  : "40",
        client_assoc.ht_cap.has_value() ? "yes" : "no",
        static_cast<uint8_t>(client_assoc.ht_cap->ht_cap_info.chan_width_set()));

    if (join_chan.cbw == CBW20) {
        debugjoin("Disable CBW40: configured to use less CBW than capability\n");
        return false;
    }
    if (join_ctx_->bss()->ht_cap == nullptr) {
        debugjoin("Disable CBW40: no HT support in target BSS\n");
        return false;
    }
    if (join_ctx_->bss()->ht_cap->ht_cap_info.chan_width_set ==
        to_enum_type(wlan_mlme::ChanWidthSet::TWENTY_ONLY)) {
        debugjoin("Disable CBW40: no CBW40 support in target BSS\n");
        return false;
    }

    if (!client_assoc.ht_cap) {
        debugjoin("Disable CBW40: no HT support in the this device\n");
        return false;
    } else if (client_assoc.ht_cap->ht_cap_info.chan_width_set() == HtCapabilityInfo::TWENTY_ONLY) {
        debugjoin("Disable CBW40: no CBW40 support in the this device\n");
        return false;
    }

    return true;
}

bool Station::IsQosReady() const {
    // TODO(NET-567,NET-599): Determine for each outbound data frame,
    // given the result of the dynamic capability negotiation, data frame
    // classification, and QoS policy.

    // Aruba / Ubiquiti are confirmed to be compatible with QoS field for the BlockAck session,
    // independently of 40MHz operation.
    return assoc_ctx_.phy == WLAN_PHY_HT || assoc_ctx_.phy == WLAN_PHY_VHT;
}

CapabilityInfo Station::OverrideCapability(CapabilityInfo cap) const {
    // parameter is of 2 bytes
    cap.set_ess(1);            // reserved in client role. 1 for better interop.
    cap.set_ibss(0);           // reserved in client role
    cap.set_cf_pollable(0);    // not supported
    cap.set_cf_poll_req(0);    // not supported
    cap.set_privacy(0);        // reserved in client role
    cap.set_spectrum_mgmt(0);  // not supported
    return cap;
}

zx_status_t Station::OverrideHtCapability(HtCapabilities* ht_cap) const {
    // TODO(porce): Determine which value to use for each field
    // (a) client radio capabilities, as reported by device driver
    // (b) intersection of (a) and radio configurations
    // (c) intersection of (b) and BSS capabilities
    // (d) intersection of (c) and radio configuration

    ZX_DEBUG_ASSERT(ht_cap != nullptr);
    if (ht_cap == nullptr) { return ZX_ERR_INVALID_ARGS; }

    HtCapabilityInfo& hci = ht_cap->ht_cap_info;
    if (!IsCbw40Rx()) { hci.set_chan_width_set(HtCapabilityInfo::TWENTY_ONLY); }

    // TODO(NET-1403): Lift up the restriction after broader interop and assoc_ctx_ adjustment.
    hci.set_tx_stbc(0);

    return ZX_OK;
}

zx_status_t Station::OverrideVhtCapability(VhtCapabilities* vht_cap,
                                           const JoinContext& join_ctx) const {
    ZX_DEBUG_ASSERT(vht_cap != nullptr);
    if (vht_cap == nullptr) { return ZX_ERR_INVALID_ARGS; }

    // See IEEE Std 802.11-2016 Table 9-250. Note zero in comparison has no name.
    VhtCapabilitiesInfo& vci = vht_cap->vht_cap_info;
    if (vci.supported_cbw_set() > 0) {
        auto cbw = join_ctx.channel().cbw;
        if (cbw != CBW160 && cbw != CBW80P80) { vht_cap->vht_cap_info.set_supported_cbw_set(0); }
    }
    return ZX_OK;
}

uint8_t Station::GetTid() {
    // IEEE Std 802.11-2016, 3.1(Traffic Identifier), 5.1.1.1 (Data Service - General), 9.4.2.30
    // (Access Policy), 9.2.4.5.2 (TID subfield) Related topics: QoS facility, TSPEC, WM, QMF, TXOP.
    // A TID is from [0, 15], and is assigned to an MSDU in the layers above the MAC.
    // [0, 7] identify Traffic Categories (TCs)
    // [8, 15] identify parameterized Traffic Streams (TSs).

    // TODO(NET-599): Implement QoS policy engine.
    return 0;
}

uint8_t Station::GetTid(const EthFrame& frame) {
    return GetTid();
}

zx_status_t Station::SetAssocContext(const MgmtFrameView<AssociationResponse>& frame) {
    ZX_DEBUG_ASSERT(join_ctx_ != nullptr);
    assoc_ctx_ = AssocContext{};
    assoc_ctx_.ts_start = timer_mgr_.Now();
    assoc_ctx_.bssid = join_ctx_->bssid();
    assoc_ctx_.aid = frame.body()->aid & kAidMask;
    assoc_ctx_.listen_interval = join_ctx_->listen_interval();

    auto assoc_resp_frame = frame.NextFrame();
    Span<const uint8_t> ie_chain = assoc_resp_frame.body_data();
    auto bss_assoc_ctx = ParseAssocRespIe(ie_chain);
    if (!bss_assoc_ctx.has_value()) {
        debugf("failed to parse AssocResp\n");
        return ZX_ERR_INVALID_ARGS;
    }
    auto ap = bss_assoc_ctx.value();
    debugjoin("rxed AssocResp:[%s]\n", debug::Describe(ap).c_str());

    ap.cap = frame.body()->cap;

    auto ifc_info = device_->GetWlanInfo().ifc_info;
    auto client = MakeClientAssocCtx(ifc_info, join_ctx_->channel());
    debugjoin("from WlanInfo: [%s]\n", debug::Describe(client).c_str());

    assoc_ctx_.cap = IntersectCapInfo(ap.cap, client.cap);
    assoc_ctx_.rates = IntersectRatesAp(ap.rates, client.rates);

    if (ap.ht_cap.has_value() && client.ht_cap.has_value()) {
        // TODO(porce): Supported MCS Set field from the outcome of the intersection
        // requires the conditional treatment depending on the value of the following fields:
        // - "Tx MCS Set Defined"
        // - "Tx Rx MCS Set Not Equal"
        // - "Tx Maximum Number Spatial Streams Supported"
        // - "Tx Unequal Modulation Supported"
        assoc_ctx_.ht_cap =
            std::make_optional(IntersectHtCap(ap.ht_cap.value(), client.ht_cap.value()));

        // Override the outcome of IntersectHtCap(), which is role agnostic.

        // If AP can't rx STBC, then the client shall not tx STBC.
        // Otherwise, the client shall do what it can do.
        if (ap.ht_cap->ht_cap_info.rx_stbc() == 0) {
            assoc_ctx_.ht_cap->ht_cap_info.set_tx_stbc(0);
        } else {
            assoc_ctx_.ht_cap->ht_cap_info.set_tx_stbc(client.ht_cap->ht_cap_info.tx_stbc());
        }

        // If AP can't tx STBC, then the client shall not expect to rx STBC.
        // Otherwise, the client shall do what it can do.
        if (ap.ht_cap->ht_cap_info.tx_stbc() == 0) {
            assoc_ctx_.ht_cap->ht_cap_info.set_rx_stbc(0);
        } else {
            assoc_ctx_.ht_cap->ht_cap_info.set_rx_stbc(client.ht_cap->ht_cap_info.rx_stbc());
        }

        assoc_ctx_.ht_op = ap.ht_op;
    }
    if (ap.vht_cap.has_value() && client.vht_cap.has_value()) {
        assoc_ctx_.vht_cap =
            std::make_optional(IntersectVhtCap(ap.vht_cap.value(), client.vht_cap.value()));
        assoc_ctx_.vht_op = ap.vht_op;
    }

    assoc_ctx_.phy = join_ctx_->phy();
    if (assoc_ctx_.ht_cap.has_value() && assoc_ctx_.ht_op.has_value()) {
        assoc_ctx_.phy = WLAN_PHY_HT;
    }
    if (assoc_ctx_.vht_cap.has_value() && assoc_ctx_.vht_op.has_value()) {
        assoc_ctx_.phy = WLAN_PHY_VHT;
    }

    // Validate if the AP accepted the requested PHY
    if (assoc_ctx_.phy != join_ctx_->phy()) {
        warnf("PHY for join (%u) and for association (%u) differ. AssocResp:[%s]", join_ctx_->phy(),
              assoc_ctx_.phy, debug::Describe(ap).c_str());
    }

    assoc_ctx_.chan = join_ctx_->channel();
    assoc_ctx_.is_cbw40_rx =
        assoc_ctx_.ht_cap &&
        ap.ht_cap->ht_cap_info.chan_width_set() == HtCapabilityInfo::TWENTY_FORTY &&
        client.ht_cap->ht_cap_info.chan_width_set() == HtCapabilityInfo::TWENTY_FORTY;

    // TODO(porce): Test capabilities and configurations of the client and its BSS.
    // TODO(porce): Ralink dependency on BlockAck, AMPDU handling
    assoc_ctx_.is_cbw40_tx = false;

    debugjoin("final AssocCtx:[%s]\n", debug::Describe(assoc_ctx_).c_str());

    return ZX_OK;
}

zx_status_t Station::NotifyAssocContext() {
    wlan_assoc_ctx_t ddk{};
    assoc_ctx_.bssid.CopyTo(ddk.bssid);
    ddk.aid = assoc_ctx_.aid;
    ddk.listen_interval = assoc_ctx_.listen_interval;
    ddk.phy = assoc_ctx_.phy;
    ddk.chan = assoc_ctx_.chan;

    auto& rates = assoc_ctx_.rates;
    ZX_DEBUG_ASSERT(rates.size() <= WLAN_MAC_MAX_RATES);
    ddk.rates_cnt = static_cast<uint8_t>(rates.size());
    std::copy(rates.cbegin(), rates.cend(), ddk.rates);

    ddk.has_ht_cap = assoc_ctx_.ht_cap.has_value();
    if (assoc_ctx_.ht_cap.has_value()) { ddk.ht_cap = assoc_ctx_.ht_cap->ToDdk(); }

    ddk.has_ht_op = assoc_ctx_.ht_op.has_value();
    if (assoc_ctx_.ht_op.has_value()) { ddk.ht_op = assoc_ctx_.ht_op->ToDdk(); }

    ddk.has_vht_cap = assoc_ctx_.vht_cap.has_value();
    if (assoc_ctx_.vht_cap.has_value()) { ddk.vht_cap = assoc_ctx_.vht_cap->ToDdk(); }

    ddk.has_vht_op = assoc_ctx_.vht_op.has_value();
    if (assoc_ctx_.vht_op.has_value()) { ddk.vht_op = assoc_ctx_.vht_op->ToDdk(); }

    return device_->ConfigureAssoc(&ddk);
}

wlan_stats::ClientMlmeStats Station::stats() const {
    return stats_.ToFidl();
}

void Station::ResetStats() {
    stats_.Reset();
}

// TODO(porce): replace SetAssocContext()
std::optional<AssocContext> Station::BuildAssocCtx(const MgmtFrameView<AssociationResponse>& frame,
                                                   const wlan_channel_t& join_chan, PHY join_phy,
                                                   uint16_t listen_interval) {
    auto assoc_resp_frame = frame.NextFrame();
    Span<const uint8_t> ie_chain = assoc_resp_frame.body_data();
    auto bssid = frame.hdr()->addr3;
    auto bss = MakeBssAssocCtx(*frame.body(), ie_chain, bssid);
    if (!bss.has_value()) { return {}; }

    auto client = MakeClientAssocCtx(device_->GetWlanInfo().ifc_info, join_chan);
    auto ctx = IntersectAssocCtx(bss.value(), client);

    // Add info that can't be derived by the intersection
    ctx.ts_start = timer_mgr_.Now();
    ctx.bssid = bss->bssid;
    ctx.aid = bss->aid;
    ctx.phy = ctx.DerivePhy();
    ctx.chan = join_chan;
    ctx.listen_interval = listen_interval;

    if (join_phy != ctx.phy) {
        // This situation is out-of specification, and may happen
        // when what the AP allowed in the Association Response
        // differs from what the AP announced in its beacon.
        // Use the outcome of the association negotiation as the AssocContext's phy.
        // TODO(porce): How should this affect the radio's channel setting?
        warnf("PHY for join (%u) and for association (%u) differ. AssocResp:[%s]", join_phy,
              ctx.DerivePhy(), debug::Describe(bss.value()).c_str());
    }

    return ctx;
}

}  // namespace wlan
