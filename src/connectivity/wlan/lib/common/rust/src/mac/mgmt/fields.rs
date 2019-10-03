// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::mac::{
        Aid, FrameControl, HtControl, MacAddr, OptionalField, Presence, ReasonCode,
        SequenceControl, StatusCode,
    },
    wlan_bitfield::bitfield,
    zerocopy::{AsBytes, FromBytes, Unaligned},
};

// IEEE Std 802.11-2016, 9.4.1.4
#[bitfield(
    0       ess,
    1       ibss,
    2       cf_pollable,
    3       cf_poll_req,
    4       privacy,
    5       short_preamble,
    6..=7   _,  // reserved
    8       spectrum_mgmt,
    9       qos,
    10      short_slot_time,
    11      apsd,
    12      radio_measurement,
    13      _,  // reserved
    14      delayed_block_ack,
    15      immediate_block_ack,
)]
#[derive(AsBytes, FromBytes, PartialEq, Eq, Clone, Copy)]
#[repr(C)]
pub struct CapabilityInfo(pub u16);

// IEEE Std 802.11-2016, 9.4.1.11, Table Table 9-47
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ActionCategory(u8);

impl ActionCategory {
    pub const SPECTRUM_MGMT: Self = Self(0);
    pub const QOS: Self = Self(1);
    pub const DLS: Self = Self(2);
    pub const BLOCK_ACK: Self = Self(3);
    pub const PUBLIC: Self = Self(4);
    pub const RADIO_MEASURE: Self = Self(5);
    pub const FT: Self = Self(6);
    pub const HT: Self = Self(7);
    pub const SA_QUERY: Self = Self(8);
    pub const PROTECTED_DUAL: Self = Self(9);
    pub const WNM: Self = Self(10);
    pub const UNPROTECTED_WNM: Self = Self(11);
    pub const TDLS: Self = Self(12);
    pub const MESH: Self = Self(13);
    pub const MULTIHOP: Self = Self(14);
    pub const SELF_PROTECTED: Self = Self(15);
    pub const DMG: Self = Self(16);
    // 17 reserved
    pub const FST: Self = Self(18);
    pub const ROBUST_AV_STREAM: Self = Self(19);
    pub const UNPROTECTED_DMG: Self = Self(20);
    pub const VHT: Self = Self(21);
    // 22 - 125 reserved
    pub const VENDOR_PROTECTED: Self = Self(126);
    pub const VENDOR: Self = Self(127);
    // 128 - 255: Error
}

// IEEE Std 802.11-2016, 9.3.3.2
#[derive(FromBytes, AsBytes, Unaligned, PartialEq, Eq, Clone, Copy, Debug)]
#[repr(C, packed)]
pub struct MgmtHdr {
    pub frame_ctrl: FrameControl,
    pub duration: u16,
    pub addr1: MacAddr,
    pub addr2: MacAddr,
    pub addr3: MacAddr,
    pub seq_ctrl: SequenceControl,
}

impl MgmtHdr {
    /// Returns the length in bytes of a mgmt header including all its fixed and optional
    /// fields (if they are present).
    pub fn len(has_ht_ctrl: Presence<HtControl>) -> usize {
        let mut bytes = std::mem::size_of::<MgmtHdr>();
        bytes += match has_ht_ctrl {
            HtControl::PRESENT => std::mem::size_of::<HtControl>(),
            HtControl::ABSENT => 0,
        };
        bytes
    }
}

// IEEE Std 802.11-2016, 9.4.1.1
#[repr(C)]
#[derive(AsBytes, FromBytes, PartialEq, Eq, Copy, Clone, Debug, Default)]
pub struct AuthAlgorithmNumber(pub u16);

impl AuthAlgorithmNumber {
    pub const OPEN: Self = Self(0);
    pub const SHARED_KEY: Self = Self(1);
    pub const FAST_BSS_TRANSITION: Self = Self(2);
    pub const SAE: Self = Self(3);
    // 4-65534 Reserved
    pub const VENDOR_SPECIFIC: Self = Self(65535);
}

// IEEE Std 802.11-2016, 9.3.3.3
#[derive(FromBytes, AsBytes, Unaligned, Clone, Copy, Debug)]
#[repr(C, packed)]
pub struct BeaconHdr {
    pub timestamp: u64,
    pub beacon_interval: u16,
    // IEEE Std 802.11-2016, 9.4.1.4
    pub capabilities: CapabilityInfo,
}

// IEEE Std 802.11-2016, 9.3.3.12
#[derive(Default, FromBytes, AsBytes, Unaligned, Clone, Copy, Debug)]
#[repr(C, packed)]
pub struct AuthHdr {
    pub auth_alg_num: AuthAlgorithmNumber,
    pub auth_txn_seq_num: u16,
    pub status_code: StatusCode,
}

// IEEE Std 802.11-2016, 9.3.3.13
#[derive(Default, FromBytes, AsBytes, Unaligned, Clone, Copy, Debug)]
#[repr(C, packed)]
pub struct DeauthHdr {
    pub reason_code: ReasonCode,
}

// IEEE Std 802.11-2016, 9.3.3.6
#[derive(FromBytes, AsBytes, Unaligned, Clone, Copy, Debug)]
#[repr(C, packed)]
pub struct AssocReqHdr {
    // IEEE Std 802.11-2016, 9.4.1.4
    pub capabilities: CapabilityInfo,
    pub listen_interval: u16,
}

// IEEE Std 802.11-2016, 9.3.3.7
#[derive(FromBytes, AsBytes, Unaligned, Clone, Copy, Debug)]
#[repr(C, packed)]
pub struct AssocRespHdr {
    // IEEE Std 802.11-2016, 9.4.1.4
    pub capabilities: CapabilityInfo,
    pub status_code: StatusCode,
    pub aid: Aid,
}
