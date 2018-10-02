// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use super::*;
use bytes::Bytes;
use crate::akm::{self, Akm};
use crate::cipher::{self, Cipher};
use crate::crypto_utils::nonce::NonceReader;
use crate::key::exchange::{
    handshake::fourway::{self, Fourway},
};
use crate::key::{ptk::Ptk, gtk::Gtk};
use crate::key_data;
use crate::key_data::kde;
use crate::{Authenticator, Supplicant};
use crate::rsna::{
    SecAssocUpdate, NegotiatedRsne, VerifiedKeyFrame
};
use crate::rsne::Rsne;
use crate::suite_selector::OUI;
use hex::FromHex;

pub const S_ADDR: [u8; 6] = [0x81, 0x76, 0x61, 0x14, 0xDF, 0xC9];
pub const A_ADDR: [u8; 6] = [0x1D, 0xE3, 0xFD, 0xDF, 0xCB, 0xD3];

pub fn get_a_rsne() -> Rsne {
    let mut rsne = Rsne::new();
    rsne.group_data_cipher_suite = Some(Cipher {
        oui: Bytes::from(&OUI[..]),
        suite_type: cipher::CCMP_128,
    });
    rsne.pairwise_cipher_suites.push(Cipher {
        oui: Bytes::from(&OUI[..]),
        suite_type: cipher::CCMP_128,
    });
    rsne.pairwise_cipher_suites.push(Cipher {
        oui: Bytes::from(&OUI[..]),
        suite_type: cipher::TKIP,
    });
    rsne.akm_suites.push(Akm {
        oui: Bytes::from(&OUI[..]),
        suite_type: akm::PSK,
    });
    rsne
}

pub fn get_rsne_bytes(rsne: &Rsne) -> Vec<u8> {
    let mut a_rsne_data = Vec::with_capacity(rsne.len());
    rsne.as_bytes(&mut a_rsne_data);
    a_rsne_data
}

pub fn get_s_rsne() -> Rsne {
    let mut rsne = Rsne::new();
    rsne.group_data_cipher_suite = Some(Cipher {
        oui: Bytes::from(&OUI[..]),
        suite_type: cipher::CCMP_128,
    });
    rsne.pairwise_cipher_suites.push(Cipher {
        oui: Bytes::from(&OUI[..]),
        suite_type: cipher::CCMP_128,
    });
    rsne.akm_suites.push(Akm {
        oui: Bytes::from(&OUI[..]),
        suite_type: akm::PSK,
    });
    rsne
}

pub fn get_supplicant() -> Supplicant {
    Supplicant::new_wpa2psk_ccmp128("ThisIsASSID".as_bytes(),
                                  "ThisIsAPassword".as_bytes(),
                                  test_util::S_ADDR,
                                  test_util::get_s_rsne(),
                                  test_util::A_ADDR,
                                  test_util::get_a_rsne())
        .expect("could not create Supplicant")
}

pub fn get_authenticator() -> Authenticator {
    Authenticator::new_wpa2psk_ccmp128("ThisIsASSID".as_bytes(),
                                       "ThisIsAPassword".as_bytes(),
                                       test_util::S_ADDR,
                                       test_util::get_s_rsne(),
                                       test_util::A_ADDR,
                                       test_util::get_a_rsne())
        .expect("could not create Authenticator")
}

pub fn get_ptk(anonce: &[u8], snonce: &[u8]) -> Ptk {
    let akm = get_akm();
    let s_rsne = get_s_rsne();
    let cipher = s_rsne
        .pairwise_cipher_suites
        .get(0)
        .expect("Supplicant's RSNE holds no Pairwise Cipher suite");
    let pmk = get_pmk();
    Ptk::new(&pmk[..], &A_ADDR, &S_ADDR, anonce, snonce, &akm, &cipher).expect("error deriving PTK")
}

pub fn get_pmk() -> Vec<u8> {
    Vec::from_hex("0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af")
        .expect("error reading PMK from hex")
}

pub fn compute_mic(kck: &[u8], frame: &eapol::KeyFrame) -> Vec<u8> {
    let akm = get_akm();
    let integrity_alg = akm
        .integrity_algorithm()
        .expect("error AKM has no known integrity Algorithm");
    let mut buf = Vec::with_capacity(frame.len());
    frame.as_bytes(true, &mut buf);
    let written = buf.len();
    buf.truncate(written);
    let mut mic: Vec<u8> = integrity_alg
        .compute(kck, &buf[..])
        .expect("error computing MIC for message");
    mic.truncate(mic_len());
    mic
}

pub fn encrypt_key_data(kek: &[u8], key_data: &[u8]) -> Vec<u8> {
    let keywrap_alg = get_akm()
        .keywrap_algorithm()
        .expect("error AKM has no known keywrap Algorithm");
    keywrap_alg
        .wrap(kek, key_data)
        .expect("could not encrypt key data")
}

pub fn mic_len() -> usize {
    get_akm().mic_bytes().expect("AKM has no known MIC size") as usize
}

pub fn get_nonce() -> Vec<u8> {
    NonceReader::new(&S_ADDR[..])
        .expect("error creating NonceReader")
        .next()
        .expect("error generating nonce")
}

pub fn get_akm() -> akm::Akm {
    get_s_rsne().akm_suites.remove(0)
}

pub fn get_pairwise_cipher() -> cipher::Cipher {
    get_s_rsne().pairwise_cipher_suites.remove(0)
}

pub fn get_4whs_msg1<F>(anonce: &[u8], msg_modifier: F) -> eapol::KeyFrame
where
    F: Fn(&mut eapol::KeyFrame),
{
    let mut msg = eapol::KeyFrame {
        version: 1,
        packet_type: 3,
        packet_body_len: 0, // Updated afterwards
        descriptor_type: 2,
        key_info: eapol::KeyInformation(0x008a),
        key_len: 16,
        key_replay_counter: 1,
        key_mic: Bytes::from(vec![0u8; mic_len()]),
        key_rsc: 0,
        key_iv: [0u8; 16],
        key_nonce: eapol::to_array(anonce),
        key_data_len: 0,
        key_data: Bytes::from(vec![]),
    };
    msg_modifier(&mut msg);
    msg.update_packet_body_len();
    msg
}

pub fn get_4whs_msg3<F>(
    ptk: &Ptk,
    anonce: &[u8],
    gtk: &[u8],
    msg_modifier: F,
) -> eapol::KeyFrame
where
    F: Fn(&mut eapol::KeyFrame),
{
    let mut buf = Vec::with_capacity(256);

    // Write GTK KDE
    let gtk_kde = kde::Gtk::new(2, kde::GtkInfoTx::BothRxTx, gtk);
    if let key_data::Element::Gtk(hdr, gtk) = gtk_kde {
        hdr.as_bytes(&mut buf);
        gtk.as_bytes(&mut buf);
    }

    // Write RSNE
    let a_rsne = get_a_rsne();
    a_rsne.as_bytes(&mut buf);

    // Add optional padding
    key_data::add_padding(&mut buf);

    // Encrypt key data
    let encrypted_key_data = encrypt_key_data(ptk.kek(), &buf[..]);

    let mut msg = eapol::KeyFrame {
        version: 1,
        packet_type: 3,
        packet_body_len: 0, // Updated afterwards
        descriptor_type: 2,
        key_info: eapol::KeyInformation(0x13ca),
        key_len: 16,
        key_replay_counter: 2,
        key_mic: Bytes::from(vec![0u8; mic_len()]),
        key_rsc: 0,
        key_iv: [0u8; 16],
        key_nonce: eapol::to_array(anonce),
        key_data_len: encrypted_key_data.len() as u16,
        key_data: Bytes::from(encrypted_key_data),
    };
    msg_modifier(&mut msg);
    msg.update_packet_body_len();

    let mic = compute_mic(ptk.kck(), &msg);
    msg.key_mic = Bytes::from(mic);

    msg
}

pub fn get_group_key_hs_msg1<F>(ptk: &Ptk, gtk: &[u8], msg_modifier: F)
    -> eapol::KeyFrame where F: Fn(&mut eapol::KeyFrame),
{
    let mut buf = Vec::with_capacity(256);

    // Write GTK KDE
    let gtk_kde = kde::Gtk::new(3, kde::GtkInfoTx::BothRxTx, gtk);
    if let key_data::Element::Gtk(hdr, gtk) = gtk_kde {
        hdr.as_bytes(&mut buf);
        gtk.as_bytes(&mut buf);
    }

    // Add optional padding
    key_data::add_padding(&mut buf);

    // Encrypt key data
    let encrypted_key_data = encrypt_key_data(ptk.kek(), &buf[..]);

    let mut msg = eapol::KeyFrame {
        version: 1,
        packet_type: 3,
        packet_body_len: 0, // Updated afterwards
        descriptor_type: 2,
        key_info: eapol::KeyInformation(0x1382),
        key_len: 0,
        key_replay_counter: 3,
        key_mic: Bytes::from(vec![0u8; mic_len()]),
        key_rsc: 0,
        key_iv: [0u8; 16],
        key_nonce: [0u8; 32],
        key_data_len: encrypted_key_data.len() as u16,
        key_data: Bytes::from(encrypted_key_data),
    };
    msg_modifier(&mut msg);
    msg.update_packet_body_len();

    let mic = compute_mic(ptk.kck(), &msg);
    msg.key_mic = Bytes::from(mic);

    msg
}

pub fn is_zero(slice: &[u8]) -> bool {
    slice.iter().all(|&x| x == 0)
}

pub fn make_fourway_cfg(role: Role) -> fourway::Config {
    fourway::Config::new(
        role,
        test_util::S_ADDR,
        test_util::get_s_rsne(),
        test_util::A_ADDR,
        test_util::get_a_rsne(),
    ).expect("could not construct PTK exchange method")
}

pub fn make_handshake(role: Role) -> Fourway {
    let pmk = test_util::get_pmk();
    Fourway::new(make_fourway_cfg(role), pmk).expect("error while creating 4-Way Handshake")
}

pub fn finalize_key_frame(mut frame: eapol::KeyFrame, kck: Option<&[u8]>) -> eapol::KeyFrame {
    frame.update_packet_body_len();

    if let Some(kck) = kck {
        let mic = compute_mic(kck, &frame);
        frame.key_mic = Bytes::from(mic);
    }

    frame
}

fn make_verified(frame: &eapol::KeyFrame, role: Role, key_replay_counter: u64)
                 -> VerifiedKeyFrame
{
    let rsne = NegotiatedRsne::from_rsne(&test_util::get_s_rsne())
        .expect("could not derive negotiated RSNE");

    let result = VerifiedKeyFrame::from_key_frame(&frame, &role, &rsne, key_replay_counter);
    assert!(result.is_ok(), "failed verifying message sent to {:?}: {}", role, result.unwrap_err());
    result.unwrap()
}

fn extract_eapol_resp(updates: &[SecAssocUpdate]) -> eapol::KeyFrame {
    updates.iter().filter_map(|u| match u {
        SecAssocUpdate::TxEapolKeyFrame(resp) => Some(resp),
        _ => None,
    }).next().expect("updates do not contain EAPOL frame").clone()
}

fn extract_reported_ptk(updates: &[SecAssocUpdate]) -> Ptk {
    updates.iter().filter_map(|u| match u {
        SecAssocUpdate::Key(Key::Ptk(ptk)) => Some(ptk),
        _ => None,
    }).next().expect("updates do not contain PTK").clone()
}

fn extract_reported_gtk(updates: &[SecAssocUpdate]) -> Gtk {
    updates.iter().filter_map(|u| match u {
        SecAssocUpdate::Key(Key::Gtk(gtk)) => Some(gtk),
        _ => None,
    }).next().expect("updates do not contain GTK").clone()
}

pub struct FourwayTestEnv {
    supplicant: Fourway,
    authenticator: Fourway,
}

impl FourwayTestEnv {
    pub fn new() -> FourwayTestEnv {
        FourwayTestEnv {
            supplicant: make_handshake(Role::Supplicant),
            authenticator: make_handshake(Role::Authenticator),
        }
    }

    pub fn initiate(&mut self, krc: u64) -> eapol::KeyFrame {
        // Initiate 4-Way Handshake. The Authenticator will send message #1 of the handshake.
        let mut a_update_sink = vec![];
        let result = self.authenticator.initiate(&mut a_update_sink, krc);
        assert!(result.is_ok(), "Authenticator failed initiating: {}", result.unwrap_err());
        assert_eq!(a_update_sink.len(), 1);

        // Verify Authenticator sent message #1.
        let msg1 = extract_eapol_resp(&a_update_sink[..]);
        msg1
    }

    pub fn send_msg1_to_supplicant(&mut self, msg1: eapol::KeyFrame, krc: u64)
                               -> (eapol::KeyFrame, Ptk)
    {
        let verified_msg1 = make_verified(&msg1, Role::Supplicant, krc);

        // Send message #1 to Supplicant and extract responses.
        let mut s_update_sink = vec![];
        let result = self.supplicant.on_eapol_key_frame(&mut s_update_sink, 0, verified_msg1);
        assert!(result.is_ok(), "Supplicant failed processing msg #1: {}", result.unwrap_err());
        let msg2 = extract_eapol_resp(&s_update_sink[..]);
        let s_ptk = extract_reported_ptk(&s_update_sink[..]);

        (msg2, s_ptk)
    }

    pub fn send_msg1_to_supplicant_expect_err(&mut self, msg1: eapol::KeyFrame, krc: u64) {
        let verified_msg1 = make_verified(&msg1, Role::Supplicant, krc);

        // Send message #1 to Supplicant and extract responses.
        let mut s_update_sink = vec![];
        let result = self.supplicant.on_eapol_key_frame(&mut s_update_sink, 0, verified_msg1);
        assert!(result.is_err(), "Supplicant successfully processed illegal msg #1");
    }

    pub fn send_msg2_to_authenticator(&mut self, msg2: eapol::KeyFrame, expected_krc: u64, next_krc: u64)
                                  -> (eapol::KeyFrame, Ptk)
    {
        let verified_msg2 = make_verified(&msg2, Role::Authenticator, expected_krc);

        // Send message #1 to Supplicant and extract responses.
        let mut a_update_sink = vec![];
        let result = self.authenticator.on_eapol_key_frame(&mut a_update_sink, next_krc, verified_msg2);
        assert!(result.is_ok(), "Authenticator failed processing msg #2: {}", result.unwrap_err());
        let msg3 = extract_eapol_resp(&a_update_sink[..]);
        let a_ptk = extract_reported_ptk(&a_update_sink[..]);

        (msg3, a_ptk)
    }

    pub fn send_msg3_to_supplicant(&mut self, msg3: eapol::KeyFrame, krc: u64)
                               -> (eapol::KeyFrame, Gtk)
    {
        let verified_msg3 = make_verified(&msg3, Role::Supplicant, krc);

        // Send message #1 to Supplicant and extract responses.
        let mut s_update_sink = vec![];
        let result = self.supplicant.on_eapol_key_frame(&mut s_update_sink, 0, verified_msg3);
        assert!(result.is_ok(), "Supplicant failed processing msg #3: {}", result.unwrap_err());
        let msg4 = extract_eapol_resp(&s_update_sink[..]);
        let s_gtk = extract_reported_gtk(&s_update_sink[..]);

        (msg4, s_gtk)
    }

    pub fn send_msg3_to_supplicant_expect_err(&mut self, msg3: eapol::KeyFrame, krc: u64) {
        let verified_msg3 = make_verified(&msg3, Role::Supplicant, krc);

        // Send message #1 to Supplicant and extract responses.
        let mut s_update_sink = vec![];
        let result = self.supplicant.on_eapol_key_frame(&mut s_update_sink, 0, verified_msg3);
        assert!(result.is_err(), "Supplicant successfully processed illegal msg #3");
    }

    pub fn send_msg4_to_authenticator(&mut self, msg4: eapol::KeyFrame, expected_krc: u64)
                                  -> Gtk
    {
        let verified_msg4 = make_verified(&msg4, Role::Authenticator, expected_krc);

        // Send message #1 to Supplicant and extract responses.
        let mut a_update_sink = vec![];
        let result = self.authenticator.on_eapol_key_frame(&mut a_update_sink, expected_krc, verified_msg4);
        assert!(result.is_ok(), "Authenticator failed processing msg #4: {}", result.unwrap_err());
        let a_gtk = extract_reported_gtk(&a_update_sink[..]);

        a_gtk
    }
}