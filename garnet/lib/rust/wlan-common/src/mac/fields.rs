// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    std::marker::PhantomData,
    wlan_bitfield::bitfield,
    zerocopy::{AsBytes, FromBytes},
};

// IEEE Std 802.11-2016, 9.2.4.1.1
#[bitfield(
    0..=1   protocol_version,
    2..=3   frame_type,
    4..=7   frame_subtype,
    8       to_ds,
    9       from_ds,
    10      more_fragments,
    11      retry,
    12      power_mgmt,
    13      more_data,
    14      protected,
    15      htc_order
)]
#[derive(AsBytes, FromBytes, PartialEq, Eq, Clone, Copy)]
#[repr(C)]
pub struct FrameControl(pub u16);

// IEEE Std 802.11-2016, 9.2.4.4
#[bitfield(
    0..=3   frag_num,
    4..=15  seq_num,
)]
#[derive(AsBytes, FromBytes, PartialEq, Eq, Clone, Copy)]
#[repr(C)]
pub struct SequenceControl(pub u16);

// IEEE Std 802.11-2016, 9.2.4.6
#[bitfield(
    0       vht,
    1..=29  middle, // see 9.2.4.6.2 for HT and 9.2.4.6.3 for VHT
    30      ac_constraint,
    31      rdg_more_ppdu,
)]
#[repr(C)]
#[derive(AsBytes, FromBytes, Copy, Clone, PartialEq, Eq)]
pub struct HtControl(pub u32);

#[derive(PartialEq, Eq)]
pub struct Presence<T: ?Sized>(bool, PhantomData<T>);

impl<T: ?Sized> Presence<T> {
    pub fn from_bool(present: bool) -> Self {
        Self(present, PhantomData)
    }
}

pub trait OptionalField {
    const PRESENT: Presence<Self> = Presence::<Self>(true, PhantomData);
    const ABSENT: Presence<Self> = Presence::<Self>(false, PhantomData);
}
impl<T: ?Sized> OptionalField for T {}
