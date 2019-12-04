// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    fidl_fuchsia_wlan_mlme as fidl_mlme, fuchsia_zircon as zx,
    log::error,
    wlan_mlme::{
        buffer::BufferProvider,
        client::{Client, ClientConfig, ClientMlme, CppChannelScheduler},
        common::{
            mac::{self, Bssid},
            sequence::SequenceManager,
        },
        device::Device,
        error::ResultExt,
        timer::*,
    },
    wlan_span::CSpan,
};

#[no_mangle]
pub extern "C" fn client_mlme_new(
    config: ClientConfig,
    device: Device,
    buf_provider: BufferProvider,
    scheduler: Scheduler,
    cpp_chan_sched: CppChannelScheduler,
) -> *mut ClientMlme {
    Box::into_raw(Box::new(ClientMlme::new(
        config,
        device,
        buf_provider,
        scheduler,
        cpp_chan_sched,
    )))
}
#[no_mangle]
pub extern "C" fn client_mlme_delete(mlme: *mut ClientMlme) {
    if !mlme.is_null() {
        unsafe { Box::from_raw(mlme) };
    }
}

#[no_mangle]
pub extern "C" fn client_sta_new(
    bssid: &[u8; 6],
    iface_mac: &[u8; 6],
    is_rsn: bool,
) -> *mut Client {
    Box::into_raw(Box::new(Client::new(Bssid(*bssid), *iface_mac, is_rsn)))
}

#[no_mangle]
pub extern "C" fn client_sta_delete(sta: *mut Client) {
    if !sta.is_null() {
        unsafe { Box::from_raw(sta) };
    }
}

#[no_mangle]
pub extern "C" fn client_mlme_timeout_fired(
    mlme: &mut ClientMlme,
    sta: *mut Client,
    event_id: EventId,
) {
    let sta = if !sta.is_null() { unsafe { Some(&mut *sta) } } else { None };
    mlme.handle_timed_event(sta, event_id);
}

#[no_mangle]
pub extern "C" fn client_mlme_seq_mgr(mlme: &mut ClientMlme) -> &mut SequenceManager {
    mlme.seq_mgr()
}

#[no_mangle]
pub extern "C" fn client_mlme_handle_mlme_msg(mlme: &mut ClientMlme, bytes: CSpan<'_>) -> i32 {
    #[allow(deprecated)] // Allow until main message loop is in Rust.
    match fidl_mlme::MlmeRequestMessage::decode(bytes.into(), &mut []) {
        Ok(msg) => mlme.handle_mlme_msg(msg).into_raw_zx_status(),
        Err(e) => {
            error!("error decoding MLME message: {}", e);
            zx::Status::IO.into_raw()
        }
    }
}

#[no_mangle]
pub extern "C" fn client_sta_send_open_auth_frame(sta: &mut Client, mlme: &mut ClientMlme) -> i32 {
    sta.send_open_auth_frame(mlme.ctx()).into_raw_zx_status()
}

#[no_mangle]
pub extern "C" fn client_sta_send_deauth_frame(
    sta: &mut Client,
    mlme: &mut ClientMlme,
    reason_code: u16,
) -> i32 {
    sta.send_deauth_frame(mlme.ctx(), mac::ReasonCode(reason_code)).into_raw_zx_status()
}

#[no_mangle]
pub extern "C" fn client_sta_send_assoc_req_frame(
    sta: &mut Client,
    mlme: &mut ClientMlme,
    cap_info: u16,
    ssid: CSpan<'_>,
    rates: CSpan<'_>,
    rsne: CSpan<'_>,
    ht_cap: CSpan<'_>,
    vht_cap: CSpan<'_>,
) -> i32 {
    sta.send_assoc_req_frame(
        mlme.ctx(),
        cap_info,
        ssid.into(),
        rates.into(),
        rsne.into(),
        ht_cap.into(),
        vht_cap.into(),
    )
    .into_raw_zx_status()
}

#[no_mangle]
pub extern "C" fn client_sta_handle_data_frame(
    sta: &mut Client,
    mlme: &mut ClientMlme,
    data_frame: CSpan<'_>,
    has_padding: bool,
    controlled_port_open: bool,
) -> i32 {
    // Safe here because |data_frame_slice| does not outlive |data_frame|.
    let data_frame_slice: &[u8] = data_frame.into();

    // TODO(42080): Do not parse here. Instead, parse in associated state only.
    match mac::MacFrame::parse(&data_frame_slice[..], has_padding) {
        Some(mac::MacFrame::Data { fixed_fields, addr4, qos_ctrl, body, .. }) => {
            sta.handle_data_frame(
                mlme.ctx(),
                &fixed_fields,
                addr4.map(|x| *x),
                qos_ctrl.map(|x| x.get()),
                body,
                controlled_port_open,
            );
        }
        // Silently discard corrupted frame.
        _ => (),
    };
    zx::sys::ZX_OK
}

#[no_mangle]
pub unsafe extern "C" fn client_sta_send_data_frame(
    sta: &mut Client,
    mlme: &mut ClientMlme,
    src: &[u8; 6],
    dest: &[u8; 6],
    is_protected: bool,
    is_qos: bool,
    ether_type: u16,
    payload: CSpan<'_>,
) -> i32 {
    sta.send_data_frame(mlme.ctx(), *src, *dest, is_protected, is_qos, ether_type, payload.into())
        .into_raw_zx_status()
}

#[no_mangle]
pub unsafe extern "C" fn client_sta_send_eapol_frame(
    sta: &mut Client,
    mlme: &mut ClientMlme,
    src: &[u8; 6],
    dest: &[u8; 6],
    is_protected: bool,
    payload: CSpan<'_>,
) {
    sta.send_eapol_frame(mlme.ctx(), *src, *dest, is_protected, payload.into())
}

#[no_mangle]
pub unsafe extern "C" fn client_sta_send_ps_poll_frame(
    sta: &mut Client,
    mlme: &mut ClientMlme,
    aid: u16,
) -> i32 {
    sta.send_ps_poll_frame(mlme.ctx(), aid).into_raw_zx_status()
}
