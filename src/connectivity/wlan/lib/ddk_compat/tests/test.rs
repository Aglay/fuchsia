// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// We link against the test data structs defined in test_data.cc, which will construct the DDK
// structs, in C++. We then check if the resulting values are expected, in Rust.

#![cfg(test)]

use wlan_ddk_compat::ddk_protocol_wlan_info;

// This links against the shared library built from test_data.cc.
#[link(name = "test_data")]
extern "C" {
    pub static test_wlan_channel: ddk_protocol_wlan_info::WlanChannel;
    pub static test_wlan_bss_config: ddk_protocol_wlan_info::WlanBssConfig;
}

#[test]
fn wlan_channel() {
    assert_eq!(unsafe { test_wlan_channel }, ddk_protocol_wlan_info::WlanChannel{
        primary: 1,
        cbw: ddk_protocol_wlan_info::WlanChannelBandwidth::_80P80,
        secondary80: 3,
    })
}

#[test]
fn wlan_bss_config() {
    assert_eq!(unsafe { test_wlan_bss_config }, ddk_protocol_wlan_info::WlanBssConfig{
        bssid: [1, 2, 3, 4, 5, 6],
        bss_type: ddk_protocol_wlan_info::WlanBssType::Personal,
        remote: true,
    })
}
