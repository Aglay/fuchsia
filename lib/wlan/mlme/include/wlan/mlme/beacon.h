// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <wlan/mlme/ht.h>
#include <wlan/mlme/mac_frame.h>
#include <wlan/mlme/ps_cfg.h>

namespace wlan {

struct BeaconConfig {
    common::MacAddr bssid;
    const uint8_t* ssid;
    size_t ssid_len;
    const uint8_t* rsne;
    size_t rsne_len;
    uint16_t beacon_period;
    wlan_channel_t channel;
    const PsCfg* ps_cfg;
    uint64_t timestamp;
    HtConfig ht;
};

zx_status_t BuildBeacon(const BeaconConfig& config, MgmtFrame<Beacon>* buffer,
                        size_t* tim_ele_offset);

zx_status_t BuildProbeResponse(const BeaconConfig& config,
                               common::MacAddr addr1,
                               MgmtFrame<ProbeResponse>* buffer);

} // namespace wlan
