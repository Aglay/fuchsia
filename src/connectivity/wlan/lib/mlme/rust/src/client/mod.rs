// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

mod utils;

use {
    crate::{
        buffer::{BufferProvider, OutBuf},
        device::{Device, TxFlags},
        error::Error,
        timer::*,
    },
    failure::format_err,
    fidl_fuchsia_wlan_mlme as fidl_mlme, fuchsia_zircon as zx,
    log::error,
    std::ffi::c_void,
    wlan_common::{
        appendable::Appendable,
        big_endian::BigEndianU16,
        buffer_writer::BufferWriter,
        frame_len,
        mac::{self, OptionalField, Presence},
        sequence::SequenceManager,
    },
    zerocopy::ByteSlice,
};

pub use utils::*;

type MacAddr = [u8; 6];
/// Maximum size of EAPOL frames forwarded to SME.
/// TODO(34845): Evaluate whether EAPOL size restriction is needed.
const MAX_EAPOL_FRAME_LEN: usize = 255;

#[derive(Debug)]
pub enum TimedEvent {}

/// A STA running in Client mode.
/// The Client STA is in its early development process and does not yet manage its internal state
/// machine or track negotiated capabilities.
pub struct ClientStation {
    device: Device,
    buf_provider: BufferProvider,
    timer: Timer<TimedEvent>,
    seq_mgr: SequenceManager,
    bssid: MacAddr,
    iface_mac: MacAddr,
}

impl ClientStation {
    pub fn new(
        device: Device,
        buf_provider: BufferProvider,
        scheduler: Scheduler,
        bssid: MacAddr,
        iface_mac: MacAddr,
    ) -> Self {
        let timer = Timer::<TimedEvent>::new(scheduler);
        Self { device, buf_provider, timer, seq_mgr: SequenceManager::new(), bssid, iface_mac }
    }

    /// Returns a reference to the STA's SNS manager.
    pub fn seq_mgr(&mut self) -> &mut SequenceManager {
        &mut self.seq_mgr
    }

    /// Extracts aggregated and non-aggregated MSDUs from the data frame.
    /// Handles all data subtypes.
    /// EAPoL MSDUs are forwarded to SME via an MLME-EAPOL.indication message independent of the
    /// STA's current controlled port status.
    /// All other MSDUs are converted into Ethernet II frames and forwarded via the device to
    /// Fuchsia's Netstack if the STA's controlled port is open.
    /// NULL-Data frames are interpreted as "Keep Alive" requests and responded with NULL data
    /// frames if the STA's controlled port is open.
    pub fn handle_data_frame<B: ByteSlice>(
        &mut self,
        bytes: B,
        has_padding: bool,
        is_controlled_port_open: bool,
    ) {
        if let Some(msdus) = mac::MsduIterator::from_raw_data_frame(bytes, has_padding) {
            match msdus {
                // Handle NULL data frames independent of the controlled port's status.
                mac::MsduIterator::Null => {
                    if let Err(e) = self.send_keep_alive_resp_frame() {
                        error!("error sending keep alive frame: {}", e);
                    }
                }
                // Handle aggregated and non-aggregated MSDUs.
                _ => {
                    for msdu in msdus {
                        let mac::Msdu { dst_addr, src_addr, llc_frame } = &msdu;
                        match llc_frame.hdr.protocol_id.to_native() {
                            // Forward EAPoL frames to SME independent of the controlled port's
                            // status.
                            mac::ETHER_TYPE_EAPOL => {
                                if let Err(e) = self.send_eapol_indication(
                                    *src_addr,
                                    *dst_addr,
                                    &llc_frame.body[..],
                                ) {
                                    error!("error sending MLME-EAPOL.indication: {}", e);
                                }
                            }
                            // Deliver non-EAPoL MSDUs only if the controlled port is open.
                            _ if is_controlled_port_open => {
                                if let Err(e) = self.deliver_msdu(msdu) {
                                    error!("error while handling data frame: {}", e);
                                }
                            }
                            // Drop all non-EAPoL MSDUs if the controlled port is closed.
                            _ => (),
                        }
                    }
                }
            }
        }
    }

    /// Delivers a single MSDU to the STA's underlying device. The MSDU is delivered as an
    /// Ethernet II frame.
    /// Returns Err(_) if writing or delivering the Ethernet II frame failed.
    fn deliver_msdu<B: ByteSlice>(&mut self, msdu: mac::Msdu<B>) -> Result<(), Error> {
        let mac::Msdu { dst_addr, src_addr, llc_frame } = msdu;

        let mut buf = [0u8; mac::MAX_ETH_FRAME_LEN];
        let mut writer = BufferWriter::new(&mut buf[..]);
        write_eth_frame(
            &mut writer,
            dst_addr,
            src_addr,
            llc_frame.hdr.protocol_id.to_native(),
            &llc_frame.body,
        )?;
        self.device
            .deliver_eth_frame(writer.into_written())
            .map_err(|s| Error::Status(format!("could not deliver Ethernet II frame"), s))
    }

    /// Sends an authentication frame using Open System authentication.
    pub fn send_open_auth_frame(&mut self) -> Result<(), Error> {
        const FRAME_LEN: usize = frame_len!(mac::MgmtHdr, mac::AuthHdr);
        let mut buf = self.buf_provider.get_buffer(FRAME_LEN)?;
        let mut w = BufferWriter::new(&mut buf[..]);
        write_open_auth_frame(&mut w, self.bssid, self.iface_mac, &mut self.seq_mgr)?;
        let bytes_written = w.bytes_written();
        let out_buf = OutBuf::from(buf, bytes_written);
        self.device
            .send_wlan_frame(out_buf, TxFlags::NONE)
            .map_err(|s| Error::Status(format!("error sending open auth frame"), s))
    }

    /// Sends a "keep alive" response to the BSS. A keep alive response is a NULL data frame sent as
    /// a response to the AP transmitting NULL data frames to the client.
    // Note: This function was introduced to meet C++ MLME feature parity. However, there needs to
    // be some investigation, whether these "keep alive" frames are the right way of keeping a
    // client associated to legacy APs.
    fn send_keep_alive_resp_frame(&mut self) -> Result<(), Error> {
        const FRAME_LEN: usize = frame_len!(mac::FixedDataHdrFields);
        let mut buf = self.buf_provider.get_buffer(FRAME_LEN)?;
        let mut w = BufferWriter::new(&mut buf[..]);
        write_keep_alive_resp_frame(&mut w, self.bssid, self.iface_mac, &mut self.seq_mgr)?;
        let bytes_written = w.bytes_written();
        let out_buf = OutBuf::from(buf, bytes_written);
        self.device
            .send_wlan_frame(out_buf, TxFlags::NONE)
            .map_err(|s| Error::Status(format!("error sending keep alive frame"), s))
    }

    /// Sends a deauthentication notification to the joined BSS with the given `reason_code`.
    pub fn send_deauth_frame(&mut self, reason_code: mac::ReasonCode) -> Result<(), Error> {
        const FRAME_LEN: usize = frame_len!(mac::MgmtHdr, mac::DeauthHdr);
        let mut buf = self.buf_provider.get_buffer(FRAME_LEN)?;
        let mut w = BufferWriter::new(&mut buf[..]);
        write_deauth_frame(&mut w, self.bssid, self.iface_mac, reason_code, &mut self.seq_mgr)?;
        let bytes_written = w.bytes_written();
        let out_buf = OutBuf::from(buf, bytes_written);
        self.device
            .send_wlan_frame(out_buf, TxFlags::NONE)
            .map_err(|s| Error::Status(format!("error sending deauthenticate frame"), s))
    }

    /// Sends the given payload as a data frame over the air.
    pub fn send_data_frame(
        &mut self,
        src: MacAddr,
        dst: MacAddr,
        is_protected: bool,
        is_qos: bool,
        ether_type: u16,
        payload: &[u8],
    ) -> Result<(), Error> {
        let qos_presence = Presence::from_bool(is_qos);
        let data_hdr_len =
            mac::FixedDataHdrFields::len(mac::Addr4::ABSENT, qos_presence, mac::HtControl::ABSENT);
        let frame_len = data_hdr_len + std::mem::size_of::<mac::LlcHdr>() + payload.len();
        let mut buf = self.buf_provider.get_buffer(frame_len)?;
        let mut w = BufferWriter::new(&mut buf[..]);
        write_data_frame(
            &mut w,
            &mut self.seq_mgr,
            self.bssid,
            src,
            dst,
            is_protected,
            is_qos,
            ether_type,
            payload,
        )?;
        let bytes_written = w.bytes_written();
        let out_buf = OutBuf::from(buf, bytes_written);
        let tx_flags = match ether_type {
            mac::ETHER_TYPE_EAPOL => TxFlags::FAVOR_RELIABILITY,
            _ => TxFlags::NONE,
        };
        self.device
            .send_wlan_frame(out_buf, tx_flags)
            .map_err(|s| Error::Status(format!("error sending data frame"), s))
    }

    /// Sends an MLME-EAPOL.indication to MLME's SME peer.
    /// Note: MLME-EAPOL.indication is a custom Fuchsia primitive and not defined in IEEE 802.11.
    fn send_eapol_indication(
        &mut self,
        src_addr: MacAddr,
        dst_addr: MacAddr,
        eapol_frame: &[u8],
    ) -> Result<(), Error> {
        if eapol_frame.len() > MAX_EAPOL_FRAME_LEN {
            return Err(Error::Internal(format_err!(
                "EAPOL frame too large: {}",
                eapol_frame.len()
            )));
        }
        self.device.access_sme_sender(|sender| {
            sender.send_eapol_ind(&mut fidl_mlme::EapolIndication {
                src_addr,
                dst_addr,
                data: eapol_frame.to_vec(),
            })
        })
    }

    /// Sends an EAPoL frame over the air and reports transmission status to SME via an
    /// MLME-EAPOL.confirm message.
    pub fn send_eapol_frame(
        &mut self,
        src: MacAddr,
        dst: MacAddr,
        is_protected: bool,
        eapol_frame: &[u8],
    ) {
        // TODO(34910): EAPoL frames can be send in QoS data frames. However, Fuchsia's old C++
        // MLME never sent EAPoL frames in QoS data frames. For feature parity do the same.
        let result = self.send_data_frame(
            src,
            dst,
            is_protected,
            false, /* don't use QoS */
            mac::ETHER_TYPE_EAPOL,
            eapol_frame,
        );
        let result_code = match result {
            Ok(()) => fidl_mlme::EapolResultCodes::Success,
            Err(e) => {
                error!("error sending EAPoL frame: {}", e);
                fidl_mlme::EapolResultCodes::TransmissionFailure
            }
        };

        // Report transmission result to SME.
        let result = self.device.access_sme_sender(|sender| {
            sender.send_eapol_conf(&mut fidl_mlme::EapolConfirm { result_code })
        });
        if let Err(e) = result {
            error!("error sending MLME-EAPOL.confirm message: {}", e);
        }
    }

    pub fn handle_timed_event(&mut self, event_id: EventId) {
        unreachable!()
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        crate::{buffer::FakeBufferProvider, device::FakeDevice},
        wlan_common::test_utils::fake_frames::*,
    };
    const BSSID: MacAddr = [6u8; 6];
    const IFACE_MAC: MacAddr = [7u8; 6];

    fn make_client_station(device: Device, scheduler: Scheduler) -> ClientStation {
        let buf_provider = FakeBufferProvider::new();
        let client = ClientStation::new(device, buf_provider, scheduler, BSSID, IFACE_MAC);
        client
    }

    #[test]
    fn client_send_open_auth_frame() {
        let mut fake_device = FakeDevice::new();
        let mut fake_scheduler = FakeScheduler::new();
        let mut client =
            make_client_station(fake_device.as_device(), fake_scheduler.as_scheduler());
        client.send_open_auth_frame().expect("error delivering WLAN frame");
        assert_eq!(fake_device.wlan_queue.len(), 1);
        #[rustfmt::skip]
        assert_eq!(&fake_device.wlan_queue[0].0[..], &[
            // Mgmt header:
            0b1011_00_00, 0b00000000, // FC
            0, 0, // Duration
            6, 6, 6, 6, 6, 6, // addr1
            7, 7, 7, 7, 7, 7, // addr2
            6, 6, 6, 6, 6, 6, // addr3
            0x10, 0, // Sequence Control
            // Auth header:
            0, 0, // auth algorithm
            1, 0, // auth txn seq num
            0, 0, // status code
        ][..]);
    }

    #[test]
    fn client_send_keep_alive_resp_frame() {
        let mut fake_device = FakeDevice::new();
        let mut fake_scheduler = FakeScheduler::new();
        let mut client =
            make_client_station(fake_device.as_device(), fake_scheduler.as_scheduler());
        client.send_keep_alive_resp_frame().expect("error delivering WLAN frame");
        assert_eq!(fake_device.wlan_queue.len(), 1);
        #[rustfmt::skip]
        assert_eq!(&fake_device.wlan_queue[0].0[..], &[
            // Data header:
            0b0100_10_00, 0b0000000_1, // FC
            0, 0, // Duration
            6, 6, 6, 6, 6, 6, // addr1
            7, 7, 7, 7, 7, 7, // addr2
            6, 6, 6, 6, 6, 6, // addr3
            0x10, 0, // Sequence Control
        ][..]);
    }

    #[test]
    fn client_send_data_frame() {
        let payload = vec![5; 8];
        let mut fake_device = FakeDevice::new();
        let mut fake_scheduler = FakeScheduler::new();
        let mut client =
            make_client_station(fake_device.as_device(), fake_scheduler.as_scheduler());
        client
            .send_data_frame([2; 6], [3; 6], false, false, 0x1234, &payload[..])
            .expect("error delivering WLAN frame");
        assert_eq!(fake_device.wlan_queue.len(), 1);
        #[rustfmt::skip]
        assert_eq!(&fake_device.wlan_queue[0].0[..], &[
            // Data header:
            0b0000_10_00, 0b0000000_1, // FC
            0, 0, // Duration
            6, 6, 6, 6, 6, 6, // addr1
            2, 2, 2, 2, 2, 2, // addr2
            3, 3, 3, 3, 3, 3, // addr3
            0x10, 0, // Sequence Control
            // LLC header:
            0xAA, 0xAA, 0x03, // DSAP, SSAP, Control
            0, 0, 0, // OUI
            0x12, 0x34, // Protocol ID
            // Payload
            5, 5, 5, 5, 5, 5, 5, 5,
        ][..]);
    }

    #[test]
    fn client_send_deauthentication_notification() {
        let mut fake_device = FakeDevice::new();
        let mut fake_scheduler = FakeScheduler::new();
        let mut client =
            make_client_station(fake_device.as_device(), fake_scheduler.as_scheduler());
        client
            .send_deauth_frame(mac::ReasonCode::AP_INITIATED)
            .expect("error delivering WLAN frame");
        assert_eq!(fake_device.wlan_queue.len(), 1);
        #[rustfmt::skip]
        assert_eq!(&fake_device.wlan_queue[0].0[..], &[
            // Mgmt header:
            0b1100_00_00, 0b00000000, // FC
            0, 0, // Duration
            6, 6, 6, 6, 6, 6, // addr1
            7, 7, 7, 7, 7, 7, // addr2
            6, 6, 6, 6, 6, 6, // addr3
            0x10, 0, // Sequence Control
            47, 0, // reason code
        ][..]);
    }

    #[test]
    fn respond_to_keep_alive_request() {
        #[rustfmt::skip]
        let data_frame = vec![
            // Data header:
            0b0100_10_00, 0b000000_1_0, // FC
            0, 0, // Duration
            6, 6, 6, 6, 6, 6, // addr1
            7, 7, 7, 7, 7, 7, // addr2
            7, 7, 7, 7, 7, 7, // addr3
            0x10, 0, // Sequence Control
        ];
        let mut fake_device = FakeDevice::new();
        let mut fake_scheduler = FakeScheduler::new();
        let mut client =
            make_client_station(fake_device.as_device(), fake_scheduler.as_scheduler());
        client.handle_data_frame(&data_frame[..], false, true);
        #[rustfmt::skip]
        assert_eq!(&fake_device.wlan_queue[0].0[..], &[
            // Data header:
            0b0100_10_00, 0b0000000_1, // FC
            0, 0, // Duration
            6, 6, 6, 6, 6, 6, // addr1
            7, 7, 7, 7, 7, 7, // addr2
            6, 6, 6, 6, 6, 6, // addr3
            0x10, 0, // Sequence Control
        ][..]);
    }

    #[test]
    fn data_frame_to_ethernet_single_llc() {
        let data_frame = make_data_frame_single_llc(None, None);
        let mut fake_device = FakeDevice::new();
        let mut fake_scheduler = FakeScheduler::new();
        let mut client =
            make_client_station(fake_device.as_device(), fake_scheduler.as_scheduler());
        client.handle_data_frame(&data_frame[..], false, true);
        assert_eq!(fake_device.eth_queue.len(), 1);
        #[rustfmt::skip]
        assert_eq!(fake_device.eth_queue[0], [
            3, 3, 3, 3, 3, 3, // dst_addr
            4, 4, 4, 4, 4, 4, // src_addr
            9, 10, // ether_type
            11, 11, 11, // payload
        ]);
    }

    #[test]
    fn data_frame_to_ethernet_amsdu() {
        let data_frame = make_data_frame_amsdu();
        let mut fake_device = FakeDevice::new();
        let mut fake_scheduler = FakeScheduler::new();
        let mut client =
            make_client_station(fake_device.as_device(), fake_scheduler.as_scheduler());
        client.handle_data_frame(&data_frame[..], false, true);
        let queue = &fake_device.eth_queue;
        assert_eq!(queue.len(), 2);
        #[rustfmt::skip]
        let mut expected_first_eth_frame = vec![
            0x78, 0x8a, 0x20, 0x0d, 0x67, 0x03, // dst_addr
            0xb4, 0xf7, 0xa1, 0xbe, 0xb9, 0xab, // src_addr
            0x08, 0x00, // ether_type
        ];
        expected_first_eth_frame.extend_from_slice(MSDU_1_PAYLOAD);
        assert_eq!(queue[0], &expected_first_eth_frame[..]);
        #[rustfmt::skip]
        let mut expected_second_eth_frame = vec![
            0x78, 0x8a, 0x20, 0x0d, 0x67, 0x04, // dst_addr
            0xb4, 0xf7, 0xa1, 0xbe, 0xb9, 0xac, // src_addr
            0x08, 0x01, // ether_type
        ];
        expected_second_eth_frame.extend_from_slice(MSDU_2_PAYLOAD);
        assert_eq!(queue[1], &expected_second_eth_frame[..]);
    }

    #[test]
    fn data_frame_to_ethernet_amsdu_padding_too_short() {
        let data_frame = make_data_frame_amsdu_padding_too_short();
        let mut fake_device = FakeDevice::new();
        let mut fake_scheduler = FakeScheduler::new();
        let mut client =
            make_client_station(fake_device.as_device(), fake_scheduler.as_scheduler());
        client.handle_data_frame(&data_frame[..], false, true);
        let queue = &fake_device.eth_queue;
        assert_eq!(queue.len(), 1);
        #[rustfmt::skip]
            let mut expected_first_eth_frame = vec![
            0x78, 0x8a, 0x20, 0x0d, 0x67, 0x03, // dst_addr
            0xb4, 0xf7, 0xa1, 0xbe, 0xb9, 0xab, // src_addr
            0x08, 0x00, // ether_type
        ];
        expected_first_eth_frame.extend_from_slice(MSDU_1_PAYLOAD);
        assert_eq!(queue[0], &expected_first_eth_frame[..]);
    }

    #[test]
    fn data_frame_controlled_port_closed() {
        let data_frame = make_data_frame_single_llc(None, None);
        let mut fake_device = FakeDevice::new();
        let mut fake_scheduler = FakeScheduler::new();
        let mut client =
            make_client_station(fake_device.as_device(), fake_scheduler.as_scheduler());
        client.handle_data_frame(&data_frame[..], false, false);

        // Verify frame was not sent to netstack.
        assert_eq!(fake_device.eth_queue.len(), 0);
    }

    #[test]
    fn eapol_frame_controlled_port_closed() {
        let (src_addr, dst_addr, eapol_frame) = make_eapol_frame();
        let mut fake_device = FakeDevice::new();
        let mut fake_scheduler = FakeScheduler::new();
        let mut client =
            make_client_station(fake_device.as_device(), fake_scheduler.as_scheduler());
        client.handle_data_frame(&eapol_frame[..], false, false);

        // Verify EAPoL frame was not sent to netstack.
        assert_eq!(fake_device.eth_queue.len(), 0);

        // Verify EAPoL frame was sent to SME.
        let eapol_ind = fake_device
            .next_mlme_msg::<fidl_mlme::EapolIndication>()
            .expect("error reading EAPOL.indication");
        assert_eq!(
            eapol_ind,
            fidl_mlme::EapolIndication { src_addr, dst_addr, data: EAPOL_PDU.to_vec() }
        );
    }

    #[test]
    fn eapol_frame_is_controlled_port_open() {
        let (src_addr, dst_addr, eapol_frame) = make_eapol_frame();
        let mut fake_device = FakeDevice::new();
        let mut fake_scheduler = FakeScheduler::new();
        let mut client =
            make_client_station(fake_device.as_device(), fake_scheduler.as_scheduler());
        client.handle_data_frame(&eapol_frame[..], false, true);

        // Verify EAPoL frame was not sent to netstack.
        assert_eq!(fake_device.eth_queue.len(), 0);

        // Verify EAPoL frame was sent to SME.
        let eapol_ind = fake_device
            .next_mlme_msg::<fidl_mlme::EapolIndication>()
            .expect("error reading EAPOL.indication");
        assert_eq!(
            eapol_ind,
            fidl_mlme::EapolIndication { src_addr, dst_addr, data: EAPOL_PDU.to_vec() }
        );
    }

    #[test]
    fn send_eapol_ind_too_large() {
        let mut fake_device = FakeDevice::new();
        let mut fake_scheduler = FakeScheduler::new();
        let mut client =
            make_client_station(fake_device.as_device(), fake_scheduler.as_scheduler());
        client
            .send_eapol_indication([1; 6], [2; 6], &[5; 256])
            .expect_err("sending too large EAPOL frame should fail");
        fake_device
            .next_mlme_msg::<fidl_mlme::EapolIndication>()
            .expect_err("expected empty channel");
    }

    #[test]
    fn send_eapol_ind_success() {
        let mut fake_device = FakeDevice::new();
        let mut fake_scheduler = FakeScheduler::new();
        let mut client =
            make_client_station(fake_device.as_device(), fake_scheduler.as_scheduler());
        client
            .send_eapol_indication([1; 6], [2; 6], &[5; 200])
            .expect("expected EAPOL.indication to be sent");
        let eapol_ind = fake_device
            .next_mlme_msg::<fidl_mlme::EapolIndication>()
            .expect("error reading EAPOL.indication");
        assert_eq!(
            eapol_ind,
            fidl_mlme::EapolIndication { src_addr: [1; 6], dst_addr: [2; 6], data: vec![5; 200] }
        );
    }

    #[test]
    fn send_eapol_frame_success() {
        let mut fake_device = FakeDevice::new();
        let mut fake_scheduler = FakeScheduler::new();
        let mut client =
            make_client_station(fake_device.as_device(), fake_scheduler.as_scheduler());
        client.send_eapol_frame(IFACE_MAC, BSSID, false, &[5; 8]);

        // Verify EAPOL.confirm message was sent to SME.
        let eapol_confirm = fake_device
            .next_mlme_msg::<fidl_mlme::EapolConfirm>()
            .expect("error reading EAPOL.confirm");
        assert_eq!(
            eapol_confirm,
            fidl_mlme::EapolConfirm { result_code: fidl_mlme::EapolResultCodes::Success }
        );

        // Verify EAPoL frame was sent over the air.
        #[rustfmt::skip]
        assert_eq!(&fake_device.wlan_queue[0].0[..], &[
            // Data header:
            0b0000_10_00, 0b0000000_1, // FC
            0, 0, // Duration
            6, 6, 6, 6, 6, 6, // addr1
            7, 7, 7, 7, 7, 7, // addr2
            6, 6, 6, 6, 6, 6, // addr3
            0x10, 0, // Sequence Control
            // LLC header:
            0xaa, 0xaa, 0x03, // dsap ssap ctrl
            0x00, 0x00, 0x00, // oui
            0x88, 0x8E, // protocol id (EAPOL)
            // EAPoL PDU:
            5, 5, 5, 5, 5, 5, 5, 5,
        ][..]);
    }

    #[test]
    fn send_eapol_frame_failure() {
        let mut fake_device = FakeDevice::new();
        let mut fake_scheduler = FakeScheduler::new();
        let mut client = make_client_station(
            fake_device.as_device_fail_wlan_tx(),
            fake_scheduler.as_scheduler(),
        );
        client.send_eapol_frame([1; 6], [2; 6], false, &[5; 200]);

        // Verify EAPOL.confirm message was sent to SME.
        let eapol_confirm = fake_device
            .next_mlme_msg::<fidl_mlme::EapolConfirm>()
            .expect("error reading EAPOL.confirm");
        assert_eq!(
            eapol_confirm,
            fidl_mlme::EapolConfirm {
                result_code: fidl_mlme::EapolResultCodes::TransmissionFailure
            }
        );

        // Verify EAPoL frame was not sent over the air.
        assert!(fake_device.wlan_queue.is_empty());
    }
}
