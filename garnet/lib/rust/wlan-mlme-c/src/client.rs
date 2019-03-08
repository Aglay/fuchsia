// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::utils,
    fuchsia_zircon::sys as zx,
    num_traits::FromPrimitive,
    wlan_mlme::{
        buffer::{BufferProvider, InBuf, OutBuf},
        client,
        common::{frame_len, mac, sequence::SequenceManager},
        device,
    },
};

#[no_mangle]
pub extern "C" fn mlme_write_open_auth_frame(
    provider: BufferProvider,
    seq_mgr: &mut SequenceManager,
    bssid: &[u8; 6],
    client_addr: &[u8; 6],
    out_buf: &mut OutBuf,
) -> i32 {
    let frame_len = frame_len!(mac::MgmtHdr, mac::AuthHdr);
    let buf_result = provider.get_buffer(frame_len);
    let mut buf = unwrap_or_bail!(buf_result, zx::ZX_ERR_NO_RESOURCES);
    let write_result = client::write_open_auth_frame(&mut buf[..], *bssid, *client_addr, seq_mgr);
    let written_bytes = unwrap_or_bail!(write_result, zx::ZX_ERR_INTERNAL).written_bytes();
    *out_buf = OutBuf::from(buf, written_bytes);
    zx::ZX_OK
}

#[no_mangle]
pub extern "C" fn mlme_write_deauth_frame(
    provider: BufferProvider,
    seq_mgr: &mut SequenceManager,
    bssid: &[u8; 6],
    client_addr: &[u8; 6],
    reason_code: u16,
    out_buf: &mut OutBuf,
) -> i32 {
    let frame_len = frame_len!(mac::MgmtHdr, mac::DeauthHdr);
    let buf_result = provider.get_buffer(frame_len);
    let mut buf = unwrap_or_bail!(buf_result, zx::ZX_ERR_NO_RESOURCES);
    let reason_code = mac::ReasonCode::from_u16(reason_code)
        .ok_or_else(|| format!("invalid reason code {}", reason_code));
    let reason_code = unwrap_or_bail!(reason_code, zx::ZX_ERR_INVALID_ARGS);
    let write_result =
        client::write_deauth_frame(&mut buf[..], *bssid, *client_addr, reason_code, seq_mgr);
    let written_bytes = unwrap_or_bail!(write_result, zx::ZX_ERR_INTERNAL).written_bytes();
    *out_buf = OutBuf::from(buf, written_bytes);
    zx::ZX_OK
}

#[no_mangle]
pub extern "C" fn mlme_write_keep_alive_resp_frame(
    provider: BufferProvider,
    seq_mgr: &mut SequenceManager,
    bssid: &[u8; 6],
    client_addr: &[u8; 6],
    out_buf: &mut OutBuf,
) -> i32 {
    let frame_len = frame_len!(mac::DataHdr);
    let buf_result = provider.get_buffer(frame_len);
    let mut buf = unwrap_or_bail!(buf_result, zx::ZX_ERR_NO_RESOURCES);
    let write_result =
        client::write_keep_alive_resp_frame(&mut buf[..], *bssid, *client_addr, seq_mgr);
    let written_bytes = unwrap_or_bail!(write_result, zx::ZX_ERR_INTERNAL).written_bytes();
    *out_buf = OutBuf::from(buf, written_bytes);
    zx::ZX_OK
}

#[no_mangle]
pub extern "C" fn mlme_deliver_eth_frame(
    device: &device::Device,
    provider: &BufferProvider,
    dst_addr: &[u8; 6],
    src_addr: &[u8; 6],
    protocol_id: u16,
    payload: *const u8,
    payload_len: usize,
) -> i32 {
    let frame_len = frame_len!(mac::EthernetIIHdr) + payload_len;
    let buf_result = provider.get_buffer(frame_len);
    let mut buf = unwrap_or_bail!(buf_result, zx::ZX_ERR_NO_RESOURCES);
    // It is safe here because `payload_slice` does not outlive `payload`.
    let payload_slice = unsafe { utils::as_slice(payload, payload_len) };
    let write_result =
        client::write_eth_frame(&mut buf[..], *dst_addr, *src_addr, protocol_id, payload_slice);
    let written_bytes = unwrap_or_bail!(write_result, zx::ZX_ERR_IO_OVERRUN).written_bytes();
    device.deliver_ethernet(&buf.as_slice()[0..written_bytes])
}

#[no_mangle]
pub unsafe extern "C" fn mlme_write_eapol_data_frame(
    provider: BufferProvider,
    seq_mgr: &mut SequenceManager,
    dest: &[u8; 6],
    src: &[u8; 6],
    is_protected: bool,
    eapol_frame_ptr: *const u8,
    eapol_frame_len: usize,
    out_buf: &mut OutBuf,
) -> i32 {
    let frame_len = frame_len!(mac::DataHdr, mac::LlcHdr) + eapol_frame_len;
    let buf_result = provider.get_buffer(frame_len);
    let mut buf = unwrap_or_bail!(buf_result, zx::ZX_ERR_NO_RESOURCES);
    let eapol_frame = utils::as_slice(eapol_frame_ptr, eapol_frame_len);
    let write_result = client::write_eapol_data_frame(
        &mut buf[..],
        *dest,
        *src,
        seq_mgr,
        is_protected,
        eapol_frame,
    );
    let written_bytes = unwrap_or_bail!(write_result, zx::ZX_ERR_INTERNAL);
    *out_buf = OutBuf::from(buf, written_bytes);
    zx::ZX_OK
}
