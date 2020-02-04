// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "sim-fake-ap.h"

#include <zircon/assert.h>

#include "src/connectivity/wlan/lib/common/cpp/include/wlan/common/status_code.h"

namespace wlan::simulation {

void FakeAp::SetChannel(const wlan_channel_t& channel) {
  // Time until next beacon.
  zx::duration diff_to_next_beacon = beacon_state_.next_beacon_time - environment_->GetTime();

  // If any station is associating with this AP, trigger channel switch.
  if (!clients_.empty() && beacon_state_.is_beaconing &&
      (CSA_beacon_interval_ >= diff_to_next_beacon)) {
    // If a new CSA is triggered, then it will override the previous one, and schedule a new channel
    // switch time.
    uint8_t cs_count = 0;
    // This is the time period start from next beacon to the end of CSA beacon interval.
    zx::duration cover = CSA_beacon_interval_ - diff_to_next_beacon;
    // This value is zero means next beacon is scheduled at the same time as CSA beacon interval
    // end, and due to the mechanism of sim_env, this beacon will be sent out because it's scheduled
    // earlier than we actually change channel.
    if (cover.get() == 0) {
      cs_count = 1;
    } else {
      cs_count =
          cover / beacon_state_.beacon_interval + (cover % beacon_state_.beacon_interval ? 1 : 0);
    }

    if (beacon_state_.is_switching_channel) {
      environment_->CancelNotification(this, beacon_state_.channel_switch_notification_id);
    }

    beacon_state_.beacon_frame_.AddCSAIE(channel, cs_count);
    beacon_state_.channel_after_CSA = channel;

    auto stop_CSAbeacon_handler = new std::function<void()>;
    *stop_CSAbeacon_handler = std::bind(&FakeAp::HandleStopCSABeaconNotification, this);
    environment_->ScheduleNotification(this, CSA_beacon_interval_,
                                       static_cast<void*>(stop_CSAbeacon_handler),
                                       &beacon_state_.channel_switch_notification_id);
    beacon_state_.is_switching_channel = true;
  } else {
    chan_ = channel;
  }
}

void FakeAp::SetBssid(const common::MacAddr& bssid) {
  bssid_ = bssid;
  beacon_state_.beacon_frame_.bssid_ = bssid;
}

void FakeAp::SetSsid(const wlan_ssid_t& ssid) {
  ssid_ = ssid;
  beacon_state_.beacon_frame_.ssid_ = ssid;
}

void FakeAp::SetCSABeaconInterval(zx::duration interval) {
  // Meaningless to set CSA_beacon_interval to 0.
  ZX_ASSERT(interval.get() != 0);
  CSA_beacon_interval_ = interval;
}

bool FakeAp::CanReceiveChannel(const wlan_channel_t& channel) {
  // For now, require an exact match
  return ((channel.primary == chan_.primary) && (channel.cbw == chan_.cbw) &&
          (channel.secondary80 == chan_.secondary80));
}

void FakeAp::ScheduleNextBeacon() {
  auto beacon_handler = new std::function<void()>;
  *beacon_handler = std::bind(&FakeAp::HandleBeaconNotification, this);
  environment_->ScheduleNotification(this, beacon_state_.beacon_interval,
                                     static_cast<void*>(beacon_handler),
                                     &beacon_state_.beacon_notification_id);
  beacon_state_.next_beacon_time = environment_->GetTime() + beacon_state_.beacon_interval;
}

void FakeAp::EnableBeacon(zx::duration beacon_period) {
  if (beacon_state_.is_beaconing) {
    // If we're already beaconing, we want to cancel any pending scheduled beacons before
    // restarting with the new beacon period.
    DisableBeacon();
  }

  // First beacon is sent out immediately
  environment_->Tx(&beacon_state_.beacon_frame_, chan_);

  beacon_state_.is_beaconing = true;
  beacon_state_.beacon_interval = beacon_period;

  ScheduleNextBeacon();
}

void FakeAp::DisableBeacon() {
  // If we stop beaconing when channel is switching, we cancel the channel switch event and directly
  // set channel to new channel.
  if (beacon_state_.is_switching_channel) {
    chan_ = beacon_state_.channel_after_CSA;
    beacon_state_.is_switching_channel = false;
    ZX_ASSERT(environment_->CancelNotification(
                  this, beacon_state_.channel_switch_notification_id) == ZX_OK);
  }

  beacon_state_.is_beaconing = false;
  ZX_ASSERT(environment_->CancelNotification(this, beacon_state_.beacon_notification_id) == ZX_OK);
}

void FakeAp::ScheduleAssocResp(uint16_t status, const common::MacAddr& dst) {
  auto handler = new std::function<void()>;
  *handler = std::bind(&FakeAp::HandleAssocRespNotification, this, status, dst);
  environment_->ScheduleNotification(this, assoc_resp_interval_, static_cast<void*>(handler));
}

void FakeAp::ScheduleProbeResp(const common::MacAddr& dst) {
  auto handler = new std::function<void()>;
  *handler = std::bind(&FakeAp::HandleProbeRespNotification, this, dst);
  environment_->ScheduleNotification(this, probe_resp_interval_, static_cast<void*>(handler));
}

void FakeAp::Rx(const SimFrame* frame, const wlan_channel_t& channel) {
  // Make sure we heard it
  if (!CanReceiveChannel(channel)) {
    return;
  }

  switch (frame->FrameType()) {
    case SimFrame::FRAME_TYPE_MGMT: {
      auto mgmt_frame = static_cast<const SimManagementFrame*>(frame);
      RxMgmtFrame(mgmt_frame);
      break;
    }

    default:
      break;
  }
}

void FakeAp::RxMgmtFrame(const SimManagementFrame* mgmt_frame) {
  switch (mgmt_frame->MgmtFrameType()) {
    case SimManagementFrame::FRAME_TYPE_PROBE_REQ: {
      auto probe_req_frame = static_cast<const SimProbeReqFrame*>(mgmt_frame);
      ScheduleProbeResp(probe_req_frame->src_addr_);
      break;
    }

    case SimManagementFrame::FRAME_TYPE_ASSOC_REQ: {
      auto assoc_req_frame = static_cast<const SimAssocReqFrame*>(mgmt_frame);

      // Ignore requests that are not for us
      if (assoc_req_frame->bssid_ != bssid_) {
        return;
      }

      if (assoc_handling_mode_ == ASSOC_IGNORED) {
        return;
      }

      if (assoc_handling_mode_ == ASSOC_REJECTED) {
        ScheduleAssocResp(WLAN_STATUS_CODE_REFUSED, assoc_req_frame->src_addr_);
        return;
      }

      // Make sure the client is not already associated
      for (auto client : clients_) {
        if (client == assoc_req_frame->src_addr_) {
          // Client is already associated
          ScheduleAssocResp(WLAN_STATUS_CODE_REFUSED_TEMPORARILY, assoc_req_frame->src_addr_);
          return;
        }
      }

      clients_.push_back(assoc_req_frame->src_addr_);
      ScheduleAssocResp(WLAN_STATUS_CODE_SUCCESS, assoc_req_frame->src_addr_);
      break;
    }

    case SimManagementFrame::FRAME_TYPE_DISASSOC_REQ: {
      auto disassoc_req_frame = static_cast<const SimDisassocReqFrame*>(mgmt_frame);
      // Ignore requests that are not for us
      if (disassoc_req_frame->dst_addr_ != bssid_) {
        return;
      }

      // Make sure the client is already associated
      for (auto client : clients_) {
        if (client == disassoc_req_frame->src_addr_) {
          // Client is already associated
          clients_.remove(disassoc_req_frame->src_addr_);
          return;
        }
      }
      break;
    }

    default:
      break;
  }
}

zx_status_t FakeAp::DisassocSta(const common::MacAddr& sta_mac, uint16_t reason) {
  // Make sure the client is already associated
  SimDisassocReqFrame disassoc_req_frame(this, bssid_, sta_mac, reason);
  for (auto client : clients_) {
    if (client == sta_mac) {
      // Client is already associated
      environment_->Tx(&disassoc_req_frame, chan_);
      clients_.remove(sta_mac);
      return ZX_OK;
    }
  }
  // client not found
  return ZX_ERR_INVALID_ARGS;
}

void FakeAp::HandleBeaconNotification() {
  ZX_ASSERT(beacon_state_.is_beaconing);
  environment_->Tx(&beacon_state_.beacon_frame_, chan_);
  // Channel switch count decrease by 1 each time after sending a CSA beacon.
  if (beacon_state_.is_switching_channel) {
    auto CSA_ie = beacon_state_.beacon_frame_.FindIE(InformationElement::IE_TYPE_CSA);
    ZX_ASSERT(static_cast<CSAInformationElement*>(CSA_ie.get())->channel_switch_count_-- > 0);
  }
  ScheduleNextBeacon();
}

void FakeAp::HandleStopCSABeaconNotification() {
  ZX_ASSERT(beacon_state_.is_beaconing);
  beacon_state_.beacon_frame_.RemoveIE(InformationElement::SimIEType::IE_TYPE_CSA);
  chan_ = beacon_state_.channel_after_CSA;
  beacon_state_.is_switching_channel = false;
}

void FakeAp::HandleAssocRespNotification(uint16_t status, common::MacAddr dst) {
  SimAssocRespFrame assoc_resp_frame(this, bssid_, dst, status);
  environment_->Tx(&assoc_resp_frame, chan_);
}

void FakeAp::HandleProbeRespNotification(common::MacAddr dst) {
  SimProbeRespFrame probe_resp_frame(this, bssid_, dst, ssid_);
  environment_->Tx(&probe_resp_frame, chan_);
}

void FakeAp::ReceiveNotification(void* payload) {
  auto handler = static_cast<std::function<void()>*>(payload);
  (*handler)();
  delete handler;
}

void FakeAp::SetAssocHandling(enum AssocHandling mode) { assoc_handling_mode_ = mode; }

}  // namespace wlan::simulation
