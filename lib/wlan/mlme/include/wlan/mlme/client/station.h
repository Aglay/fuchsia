// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_LIB_WLAN_MLME_INCLUDE_WLAN_MLME_CLIENT_STATION_H_
#define GARNET_LIB_WLAN_MLME_INCLUDE_WLAN_MLME_CLIENT_STATION_H_

#include <wlan/mlme/assoc_context.h>
#include <wlan/mlme/client/channel_scheduler.h>
#include <wlan/mlme/client/join_context.h>
#include <wlan/mlme/device_interface.h>
#include <wlan/mlme/eapol.h>
#include <wlan/mlme/mac_frame.h>
#include <wlan/mlme/packet.h>
#include <wlan/mlme/sequence.h>
#include <wlan/mlme/service.h>
#include <wlan/mlme/timer_manager.h>

#include <fuchsia/wlan/mlme/cpp/fidl.h>
#include <fuchsia/wlan/stats/cpp/fidl.h>

#include <fbl/unique_ptr.h>
#include <wlan/common/macaddr.h>
#include <wlan/common/moving_average.h>
#include <wlan/common/stats.h>
#include <wlan/protocol/mac.h>
#include <zircon/types.h>

#include <optional>
#include <vector>

namespace wlan {

class Packet;

class Station {
   public:
    Station(DeviceInterface* device, TimerManager&& timer_mgr, ChannelScheduler* chan_sched,
            JoinContext* join_ctx);

    enum class WlanState {
        kIdle,
        kAuthenticating,
        kAuthenticated,
        kAssociated,
        // 802.1X's controlled port is not handled here.
    };

    void Reset();

    zx_status_t SendKeepAliveResponse();

    zx_status_t HandleAnyMlmeMsg(const BaseMlmeMsg&);
    zx_status_t HandleEthFrame(EthFrame&&);
    zx_status_t HandleAnyWlanFrame(fbl::unique_ptr<Packet>);
    zx_status_t HandleTimeout();

    void PreSwitchOffChannel();
    void BackToMainChannel();

    ::fuchsia::wlan::stats::ClientMlmeStats stats() const;
    void ResetStats();

   private:
    static constexpr size_t kAssocBcnCountTimeout = 20;
    static constexpr size_t kSignalReportBcnCountTimeout = 10;
    static constexpr size_t kAutoDeauthBcnCountTimeout = 100;
    static constexpr zx::duration kOnChannelTimeAfterSend = zx::msec(500);
    // Maximum number of packets buffered while station is in power saving mode.
    // TODO(NET-687): Find good BU limit.
    static constexpr size_t kMaxPowerSavingQueueSize = 30;

    zx_status_t HandleAnyMgmtFrame(MgmtFrame<>&&);
    zx_status_t HandleAnyDataFrame(DataFrame<>&&);
    bool ShouldDropMgmtFrame(const MgmtFrameView<>&);
    void HandleBeacon(MgmtFrame<Beacon>&&);
    zx_status_t HandleAuthentication(MgmtFrame<Authentication>&&);
    zx_status_t HandleDeauthentication(MgmtFrame<Deauthentication>&&);
    zx_status_t HandleAssociationResponse(MgmtFrame<AssociationResponse>&&);
    zx_status_t HandleDisassociation(MgmtFrame<Disassociation>&&);
    zx_status_t HandleActionFrame(MgmtFrame<ActionFrame>&&);
    bool ShouldDropDataFrame(const DataFrameView<>&);
    zx_status_t HandleNullDataFrame(DataFrame<NullDataHdr>&& frame);
    zx_status_t HandleDataFrame(DataFrame<LlcHeader>&& frame);
    zx_status_t HandleLlcFrame(const FrameView<LlcHeader>& llc_frame, size_t llc_payload_len,
                               const common::MacAddr& src, const common::MacAddr& dest);
    zx_status_t HandleAmsduFrame(DataFrame<AmsduSubframeHeader>&&);
    zx_status_t HandleAddBaRequest(const AddBaRequestFrame&);

    zx_status_t HandleMlmeJoinReq(const MlmeMsg<::fuchsia::wlan::mlme::JoinRequest>& req);
    zx_status_t HandleMlmeAuthReq(const MlmeMsg<::fuchsia::wlan::mlme::AuthenticateRequest>& req);
    zx_status_t HandleMlmeDeauthReq(
        const MlmeMsg<::fuchsia::wlan::mlme::DeauthenticateRequest>& req);
    zx_status_t HandleMlmeAssocReq(const MlmeMsg<::fuchsia::wlan::mlme::AssociateRequest>& req);
    zx_status_t HandleMlmeEapolReq(const MlmeMsg<::fuchsia::wlan::mlme::EapolRequest>& req);
    zx_status_t HandleMlmeSetKeysReq(const MlmeMsg<::fuchsia::wlan::mlme::SetKeysRequest>& req);

    zx_status_t SendAddBaRequestFrame();

    // Send a non-data frame
    zx_status_t SendCtrlFrame(fbl::unique_ptr<Packet> packet, CBW cbw, PHY phy);
    zx_status_t SendMgmtFrame(fbl::unique_ptr<Packet> packet);
    zx_status_t SetPowerManagementMode(bool ps_mode);
    zx_status_t SendPsPoll();
    zx_status_t SendDeauthFrame(::fuchsia::wlan::mlme::ReasonCode reason_code);
    void SendBufferedUnits();
    zx_status_t SendWlan(fbl::unique_ptr<Packet> packet, CBW cbw, PHY phy, uint32_t flags = 0);
    void DumpDataFrame(const DataFrameView<>&);

    zx::time deadline_after_bcn_period(size_t bcn_count);
    zx::duration FullAutoDeauthDuration();

    // Returns the STA's own MAC address.
    const common::MacAddr& self_addr() const { return device_->GetState()->address(); }

    bool IsCbw40Rx() const;
    bool IsQosReady() const;

    CapabilityInfo OverrideCapability(CapabilityInfo cap) const;
    zx_status_t OverrideHtCapability(HtCapabilities* htc) const;
    zx_status_t OverrideVhtCapability(VhtCapabilities* vht_cap, const JoinContext& join_ctx) const;
    uint8_t GetTid();
    uint8_t GetTid(const EthFrame& frame);
    zx_status_t SetAssocContext(const MgmtFrameView<AssociationResponse>& resp);
    std::optional<AssocContext> BuildAssocCtx(const MgmtFrameView<AssociationResponse>& frame,
                                              const wlan_channel_t& join_chan, PHY join_phy,
                                              uint16_t listen_interval);

    zx_status_t NotifyAssocContext();

    DeviceInterface* device_;
    TimerManager timer_mgr_;
    ChannelScheduler* chan_sched_;
    Sequence seq_;
    JoinContext* join_ctx_;

    WlanState state_ = WlanState::kIdle;
    TimedEvent auth_timeout_;
    TimedEvent assoc_timeout_;
    TimedEvent signal_report_timeout_;
    TimedEvent auto_deauth_timeout_;
    // The remaining time we'll wait for a beacon before deauthenticating (while we are on channel)
    // Note: Off-channel time does not count against `remaining_auto_deauth_timeout_`
    zx::duration remaining_auto_deauth_timeout_ = zx::duration::infinite();
    // The last time we re-calculated the `remaining_auto_deauth_timeout_`
    // Note: During channel switching, `auto_deauth_last_accounted_` is set to the timestamp
    //       we go back on channel (to make computation easier).
    zx::time auto_deauth_last_accounted_;

    common::MovingAverageDbm<20> avg_rssi_dbm_;
    AuthAlgorithm auth_alg_ = AuthAlgorithm::kOpenSystem;
    eapol::PortState controlled_port_ = eapol::PortState::kBlocked;

    common::WlanStats<common::ClientMlmeStats, ::fuchsia::wlan::stats::ClientMlmeStats> stats_;
    AssocContext assoc_ctx_{};

    // Queue holding buffered, outbound Ethernet frames
    PacketQueue bu_queue_;
};

}  // namespace wlan

#endif  // GARNET_LIB_WLAN_MLME_INCLUDE_WLAN_MLME_CLIENT_STATION_H_
