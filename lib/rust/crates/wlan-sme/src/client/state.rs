// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use client::bss::convert_bss_description;
use fidl_mlme::{self, BssDescription, MlmeEvent};
use client::{ConnectResult, Status, Tokens};
use client::internal::{MlmeSink, UserSink};
use MlmeRequest;
use super::DeviceInfo;
use wlan_rsn::rsne::Rsne;
use wlan_rsn::key::exchange::Key;
use wlan_rsn::rsna::{esssa::EssSa, SecAssocUpdate, SecAssocStatus};
use eapol;

const DEFAULT_JOIN_FAILURE_TIMEOUT: u32 = 20; // beacon intervals
const DEFAULT_AUTH_FAILURE_TIMEOUT: u32 = 20; // beacon intervals

#[derive(Debug)]
pub enum LinkState<T: Tokens> {
    EstablishingRsna(Option<T::ConnectToken>, Rsna),
    LinkUp(Option<Rsna>)
}

#[derive(Debug)]
pub struct ConnectCommand<T> {
    pub bss: Box<BssDescription>,
    pub token: Option<T>,
    pub rsna: Option<Rsna>,
}

#[derive(Debug)]
pub struct Rsna {
    s_rsne: Rsne,
    esssa: EssSa,
}

impl Rsna {
    pub fn new(s_rsne: Rsne, esssa: EssSa) -> Rsna {
        assert_eq!(s_rsne.pairwise_cipher_suites.len(), 1);
        assert_eq!(s_rsne.akm_suites.len(), 1);
        assert!(s_rsne.group_data_cipher_suite.is_some());

        Rsna {s_rsne, esssa}
    }
}

#[derive(Debug)]
pub enum RsnaStatus {
    Established,
    Failed(ConnectResult),
    Unchanged,
}

#[derive(Debug)]
pub enum State<T: Tokens> {
    Idle,
    Joining {
        cmd: ConnectCommand<T::ConnectToken>,
    },
    Authenticating {
        cmd: ConnectCommand<T::ConnectToken>,
    },
    Associating {
        cmd: ConnectCommand<T::ConnectToken>,
    },
    Associated {
        bss: Box<BssDescription>,
        last_rssi: Option<i8>,
        link_state: LinkState<T>,
    },
}

impl<T: Tokens> State<T> {
    pub fn on_mlme_event(self, _device_info: &DeviceInfo, event: MlmeEvent, mlme_sink: &MlmeSink,
                         user_sink: &UserSink<T>) -> Self {
        match self {
            State::Idle => {
                warn!("Unexpected MLME message while Idle: {:?}", event);
                State::Idle
            },
            State::Joining{ cmd } => match event {
                MlmeEvent::JoinConf { resp } => match resp.result_code {
                    fidl_mlme::JoinResultCodes::Success => {
                        mlme_sink.send(MlmeRequest::Authenticate(
                            fidl_mlme::AuthenticateRequest {
                                peer_sta_address: cmd.bss.bssid.clone(),
                                auth_type: fidl_mlme::AuthenticationTypes::OpenSystem,
                                auth_failure_timeout: DEFAULT_AUTH_FAILURE_TIMEOUT,
                            }));
                        State::Authenticating { cmd }
                    },
                    other => {
                        error!("Join request failed with result code {:?}", other);
                        report_connect_finished(cmd.token, user_sink, ConnectResult::Failed);
                        State::Idle
                    }
                },
                _ => {
                    State::Joining{ cmd }
                }
            },
            State::Authenticating{ cmd } => match event {
                MlmeEvent::AuthenticateConf { resp } => match resp.result_code {
                    fidl_mlme::AuthenticateResultCodes::Success => {
                        to_associating_state(cmd, mlme_sink)
                    },
                    other => {
                        error!("Authenticate request failed with result code {:?}", other);
                        report_connect_finished(cmd.token, user_sink, ConnectResult::Failed);
                        State::Idle
                    }
                },
                _ => State::Authenticating{ cmd }
            },
            State::Associating{ cmd } => match event {
                MlmeEvent::AssociateConf { resp } => match resp.result_code {
                    fidl_mlme::AssociateResultCodes::Success => match cmd.rsna {
                        Some(rsna) => {
                            State::Associated {
                                bss: cmd.bss,
                                last_rssi: None,
                                link_state: LinkState::EstablishingRsna(cmd.token, rsna),
                            }
                        },
                        None => {
                            report_connect_finished(cmd.token, user_sink, ConnectResult::Success);
                            State::Associated {
                                bss: cmd.bss,
                                last_rssi: None,
                                link_state: LinkState::LinkUp(None),
                            }
                        }
                    },
                    other => {
                        error!("Associate request failed with result code {:?}", other);
                        report_connect_finished(cmd.token, user_sink, ConnectResult::Failed);
                        State::Idle
                    }
                },
                _ => State::Associating{ cmd }
            },
            State::Associated { bss, last_rssi, link_state } => match event {
                MlmeEvent::DisassociateInd{ .. } => {
                    let (token, mut rsna) = match link_state {
                        LinkState::LinkUp(rsna) => (None, rsna),
                        LinkState::EstablishingRsna(token, rsna) => (token, Some(rsna)),
                    };
                    // Client is disassociating. The ESS-SA must be kept alive but reset.
                    if let Some(rsna) = &mut rsna {
                        rsna.esssa.reset();
                    }
                    let cmd = ConnectCommand{ bss, token, rsna };
                    to_associating_state(cmd, mlme_sink)
                },
                MlmeEvent::DeauthenticateInd{ ind } => {
                    if let LinkState::EstablishingRsna(token, _) = link_state {
                        let connect_result = deauth_code_to_connect_result(ind.reason_code);
                        report_connect_finished(token, user_sink, connect_result);
                    }
                    State::Idle
                },
                MlmeEvent::SignalReport{ ind } => {
                    State::Associated {
                        bss,
                        last_rssi: Some(ind.rssi_dbm),
                        link_state,
                    }
                },
                MlmeEvent::EapolInd{ ref ind } if bss.rsn.is_some() => match link_state {
                    LinkState::EstablishingRsna(token, mut rsna) => {
                        match process_eapol_ind(mlme_sink, &mut rsna, &ind) {
                            RsnaStatus::Established => {
                                report_connect_finished(token, user_sink, ConnectResult::Success);
                                let link_state = LinkState::LinkUp(Some(rsna));
                                State::Associated { bss, last_rssi, link_state }
                            },
                            RsnaStatus::Failed(result) => {
                                report_connect_finished(token, user_sink, result);
                                send_deauthenticate_request(bss, mlme_sink);
                                State::Idle
                            },
                            RsnaStatus::Unchanged => {
                                let link_state = LinkState::EstablishingRsna(token, rsna);
                                State::Associated { bss, last_rssi, link_state }
                            },
                        }
                    },
                    LinkState::LinkUp(Some(mut rsna)) => {
                        match process_eapol_ind(mlme_sink, &mut rsna, &ind) {
                            RsnaStatus::Unchanged => {},
                            // Once re-keying is supported, the RSNA can fail in LinkUp as well
                            // and cause deauthentication.
                            s => error!("unexpected RsnaStatus in LinkUp state: {:?}", s),
                        };
                        let link_state = LinkState::LinkUp(Some(rsna));
                        State::Associated { bss, last_rssi, link_state }
                    },
                    _ => panic!("expected Link to carry RSNA because bss.rsn is present"),
                }
                _ => State::Associated{ bss, last_rssi, link_state }
            },
        }
    }

    pub fn connect(self, cmd: ConnectCommand<T::ConnectToken>,
                   mlme_sink: &MlmeSink, user_sink: &UserSink<T>) -> Self {
        self.disconnect_internal(mlme_sink, user_sink);
        mlme_sink.send(MlmeRequest::Join(
            fidl_mlme::JoinRequest {
                selected_bss: clone_bss_desc(&cmd.bss),
                join_failure_timeout: DEFAULT_JOIN_FAILURE_TIMEOUT,
                nav_sync_delay: 0,
                op_rate_set: vec![]
            }
        ));
        State::Joining { cmd }
    }

    fn disconnect_internal(self, mlme_sink: &MlmeSink, user_sink: &UserSink<T>) {
        match self {
            State::Idle => { },
            State::Joining { cmd } | State::Authenticating { cmd }  => {
                report_connect_finished(cmd.token, user_sink, ConnectResult::Canceled);
            },
            State::Associating{ cmd, .. } => {
                report_connect_finished(cmd.token, user_sink, ConnectResult::Canceled);
                send_deauthenticate_request(cmd.bss, mlme_sink);
            },
            State::Associated { bss, .. } => {
                send_deauthenticate_request(bss, mlme_sink);
            },
        }
    }

    pub fn status(&self) -> Status {
        match self {
            State::Idle => Status {
                connected_to: None,
                connecting_to: None,
            },
            State::Joining { cmd }
                | State::Authenticating { cmd }
                | State::Associating { cmd, .. } =>
            {
                Status {
                    connected_to: None,
                    connecting_to: Some(cmd.bss.ssid.as_bytes().to_vec()),
                }
            },
            State::Associated { bss, link_state: LinkState::EstablishingRsna(..), .. } => Status {
                connected_to: None,
                connecting_to: Some(bss.ssid.as_bytes().to_vec()),
            },
            State::Associated { bss, link_state: LinkState::LinkUp(..), .. } => Status {
                connected_to: Some(convert_bss_description(bss)),
                connecting_to: None,
            },
        }
    }
}

fn deauth_code_to_connect_result(reason_code: fidl_mlme::ReasonCode) -> ConnectResult {
    match reason_code {
        fidl_mlme::ReasonCode::InvalidAuthentication
        | fidl_mlme::ReasonCode::Ieee8021XAuthFailed => ConnectResult::BadCredentials,
        _ => ConnectResult::Failed
    }
}

fn process_eapol_ind(mlme_sink: &MlmeSink, rsna: &mut Rsna, ind: &fidl_mlme::EapolIndication)
    -> RsnaStatus
{
    let mic_len = get_mic_size(&rsna.s_rsne);
    let eapol_pdu = &ind.data[..];
    let eapol_frame = match eapol::key_frame_from_bytes(eapol_pdu, mic_len).to_full_result() {
        Ok(key_frame) => Some(eapol::Frame::Key(key_frame)),
        Err(e) => {
            error!("received invalid EAPOL Key frame: {:?}", e);
            None
        }
    };

    if let Some(eapol_frame) = eapol_frame {
        let bssid = ind.src_addr;
        let sta_addr = ind.dst_addr;
        match rsna.esssa.on_eapol_frame(&eapol_frame) {
            Ok(updates) => for update in updates {
                match update {
                    // ESS Security Association requests to send an EAPOL frame.
                    // Forward EAPOL frame to MLME.
                    SecAssocUpdate::TxEapolKeyFrame(frame) => {
                        send_eapol_frame(mlme_sink, bssid, sta_addr, frame)
                    },
                    // ESS Security Association derived a new key.
                    // Configure key in MLME.
                    SecAssocUpdate::Key(key) => {
                        send_keys(mlme_sink, bssid, &rsna.s_rsne, key)
                    },
                    // Received a status update.
                    // TODO(hahnr): Rework this part.
                    // As of now, we depend on the fact that the status is always the last update.
                    // However, this fact is not clear from the API.
                    // We should fix the API and make this more explicit.
                    // Then we should rework this part.
                    SecAssocUpdate::Status(status) => match status {
                        // ESS Security Association was successfully established. Link is now up.
                        SecAssocStatus::EssSaEstablished => return RsnaStatus::Established,
                        SecAssocStatus::WrongPassword => {
                            return RsnaStatus::Failed(ConnectResult::BadCredentials);
                        }
                    },
                }
            }
            Err(e) => error!("error processing EAPOL key frame: {}", e),
        };
    }

    RsnaStatus::Unchanged
}

fn get_mic_size(s_rsne: &Rsne) -> u16
{
    // The client should never authenticate with a BSS which uses an AKM with an unknown MIC size.
    s_rsne.akm_suites.iter().next().expect("expected RSNE to carry exactly one AKM suite")
        .mic_bytes().expect("expected AKM to have a known MIC size")
}

fn send_eapol_frame(mlme_sink: &MlmeSink, bssid: [u8; 6], sta_addr: [u8; 6], frame: eapol::KeyFrame)
{
    let mut buf = Vec::with_capacity(frame.len());
    frame.as_bytes(false, &mut buf);
    mlme_sink.send(MlmeRequest::Eapol(
        fidl_mlme::EapolRequest {
            src_addr: sta_addr,
            dst_addr: bssid,
            data: buf,
        }
    ));
}

fn send_keys(mlme_sink: &MlmeSink, bssid: [u8; 6], s_rsne: &Rsne, key: Key)
{
    match key {
        Key::Ptk(ptk) => {
            let pairwise = s_rsne.pairwise_cipher_suites.iter().next()
                .expect("expected RSNE to carry exactly one pairwise cipher suite");
            mlme_sink.send(MlmeRequest::SetKeys(
                fidl_mlme::SetKeysRequest {
                    keylist: vec![fidl_mlme::SetKeyDescriptor{
                        key_type: fidl_mlme::KeyType::Pairwise,
                        key: ptk.tk().to_vec(),
                        key_id: 0,
                        address: bssid,
                        cipher_suite_oui: eapol::to_array(&pairwise.oui[..]),
                        cipher_suite_type: pairwise.suite_type,
                        rsc: [0u8; 8],
                    }]
                }
            ));
        },
        Key::Gtk(gtk) => {
            let group_data = s_rsne.group_data_cipher_suite.as_ref()
                .expect("expected RSNE to carry a group data cipher suite");
            mlme_sink.send(MlmeRequest::SetKeys(
                fidl_mlme::SetKeysRequest {
                    keylist: vec![fidl_mlme::SetKeyDescriptor{
                        key_type: fidl_mlme::KeyType::Group,
                        key: gtk.tk().to_vec(),
                        key_id: gtk.key_id() as u16,
                        address: [0xFFu8; 6],
                        cipher_suite_oui: eapol::to_array(&group_data.oui[..]),
                        cipher_suite_type: group_data.suite_type,
                        rsc: [0u8; 8],
                    }]
                }
            ));
        },
        _ => error!("derived unexpected key")
    };
}

fn send_deauthenticate_request(current_bss: Box<BssDescription>,
                               mlme_sink: &MlmeSink) {
    mlme_sink.send(MlmeRequest::Deauthenticate(
        fidl_mlme::DeauthenticateRequest {
            peer_sta_address: current_bss.bssid.clone(),
            reason_code: fidl_mlme::ReasonCode::StaLeaving,
        }
    ));
}

fn to_associating_state<T>(cmd: ConnectCommand<T::ConnectToken>, mlme_sink: &MlmeSink)
    -> State<T>
    where T: Tokens
{
    let s_rsne_data = cmd.rsna.as_ref().map(|rsna| {
        let mut buf = Vec::with_capacity(rsna.s_rsne.len());
        rsna.s_rsne.as_bytes(&mut buf);
        buf
    });

    mlme_sink.send(MlmeRequest::Associate(
        fidl_mlme::AssociateRequest {
            peer_sta_address: cmd.bss.bssid.clone(),
            rsn: s_rsne_data,
        }
    ));
    State::Associating { cmd }
}

fn report_connect_finished<T>(token: Option<T::ConnectToken>,
                              user_sink: &UserSink<T>, result: ConnectResult)
    where T: Tokens
{
    if let Some(token) = token {
        user_sink.send(super::UserEvent::ConnectFinished {
            token,
            result
        })
    }
}

fn clone_ht_capabilities(c: &fidl_mlme::HtCapabilities) -> fidl_mlme::HtCapabilities {
    fidl_mlme::HtCapabilities {
        ht_cap_info: fidl_mlme::HtCapabilityInfo { ..c.ht_cap_info },
        ampdu_params: fidl_mlme::AmpduParams { ..c.ampdu_params },
        mcs_set: fidl_mlme::SupportedMcsSet { ..c.mcs_set },
        ht_ext_cap: fidl_mlme::HtExtCapabilities { ..c.ht_ext_cap },
        txbf_cap: fidl_mlme::TxBfCapability { ..c.txbf_cap },
        asel_cap: fidl_mlme::AselCapability { ..c.asel_cap },
    }
}

fn clone_ht_operation(o: &fidl_mlme::HtOperation) -> fidl_mlme::HtOperation {
    fidl_mlme::HtOperation {
        ht_op_info: fidl_mlme::HtOperationInfo { ..o.ht_op_info },
        basic_mcs_set: fidl_mlme::SupportedMcsSet { ..o.basic_mcs_set },
        ..*o
    }
}

fn clone_vht_mcs_nss(m: &fidl_mlme::VhtMcsNss) -> fidl_mlme::VhtMcsNss {
    fidl_mlme::VhtMcsNss {
        rx_max_mcs: m.rx_max_mcs.clone(),
        tx_max_mcs: m.tx_max_mcs.clone(),
        ..*m
    }
}

fn clone_vht_capabilities_info(i: &fidl_mlme::VhtCapabilitiesInfo) -> fidl_mlme::VhtCapabilitiesInfo {
    fidl_mlme::VhtCapabilitiesInfo {
        ..*i
    }
}

fn clone_vht_capabilities(c: &fidl_mlme::VhtCapabilities) -> fidl_mlme::VhtCapabilities {
    fidl_mlme::VhtCapabilities {
        vht_cap_info: clone_vht_capabilities_info(&c.vht_cap_info),
        vht_mcs_nss: clone_vht_mcs_nss(&c.vht_mcs_nss)
    }
}

fn clone_vht_operation(o: &fidl_mlme::VhtOperation) -> fidl_mlme::VhtOperation {
    fidl_mlme::VhtOperation {
        vht_mcs_nss: clone_vht_mcs_nss(&o.vht_mcs_nss),
        ..*o
    }
}

fn clone_bss_desc(d: &fidl_mlme::BssDescription) -> fidl_mlme::BssDescription {
    fidl_mlme::BssDescription {
        bssid: d.bssid.clone(),
        ssid: d.ssid.clone(),
        bss_type: d.bss_type,
        beacon_period: d.beacon_period,
        dtim_period: d.dtim_period,
        timestamp: d.timestamp,
        local_time: d.local_time,

        country: d.country.clone(),
        cap: fidl_mlme::CapabilityInfo { ..d.cap },

        rsn: d.rsn.clone(),

        rcpi_dbmh: d.rcpi_dbmh,
        rsni_dbh: d.rsni_dbh,

        ht_cap: d.ht_cap.as_ref().map(|v| Box::new(clone_ht_capabilities(v))),
        ht_op:  d.ht_op.as_ref().map(|v| Box::new(clone_ht_operation(v))),

        vht_cap: d.vht_cap.as_ref().map(|v| Box::new(clone_vht_capabilities(v))),
        vht_op:  d.vht_op.as_ref().map(|v| Box::new(clone_vht_operation(v))),

        chan: fidl_mlme::WlanChan {
            primary: d.chan.primary,
            cbw: d.chan.cbw,
            secondary80: d.chan.secondary80,
        },
        rssi_dbm: d.rssi_dbm,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use futures::channel::mpsc;
    use client::test_utils::fake_bss_description;
    use std::collections::HashSet;
    use std::iter::FromIterator;
    use Ssid;

    #[derive(Debug)]
    struct FakeTokens;

    impl Tokens for FakeTokens {
        type ScanToken = u32;
        type ConnectToken = u32;
    }

    #[test]
    fn associate_happy_path_unprotected() {
        let state = State::Idle::<FakeTokens>;
        let (mlme_sink, mut mlme_stream) = mpsc::unbounded();
        let (user_sink, mut _user_stream) = mpsc::unbounded();
        let mlme_sink = MlmeSink{ sink: mlme_sink };
        let user_sink = UserSink{ sink: user_sink };
        let device_info = fake_device_info();

        // Issue a "connect" command
        let cmd = ConnectCommand {
            bss: Box::new(bss(b"foo".to_vec(), [7, 7, 7, 7, 7, 7])),
            token: Some(123_u32),
            rsna: None,
        };
        let state = state.connect(cmd, &mlme_sink, &user_sink);

        // (sme->mlme) Expect a JoinRequest
        match mlme_stream.try_next().unwrap() {
            Some(MlmeRequest::Join(req)) => {
                assert_eq!("foo", &req.selected_bss.ssid);
            },
            other => panic!("expected a Join request, got {:?}", other),
        }

        // (mlme->sme) Send a JoinConf as a response
        let join_conf = MlmeEvent::JoinConf {
            resp: fidl_mlme::JoinConfirm {
                result_code: fidl_mlme::JoinResultCodes::Success
            }
        };
        let state = state.on_mlme_event(&device_info, join_conf, &mlme_sink, &user_sink);

        // (sme->mlme) Expect an AuthenticateRequest
        match mlme_stream.try_next().unwrap() {
            Some(MlmeRequest::Authenticate(req)) => {
                assert_eq!([ 7, 7, 7, 7, 7, 7 ], req.peer_sta_address);
            },
            other => panic!("expected an Authenticate request, got {:?}", other)
        }

        // (mlme->sme) Send an AuthenticateConf as a response
        let auth_conf = MlmeEvent::AuthenticateConf {
            resp: fidl_mlme::AuthenticateConfirm {
                peer_sta_address: [7, 7, 7, 7, 7, 7],
                auth_type: fidl_mlme::AuthenticationTypes::OpenSystem,
                result_code: fidl_mlme::AuthenticateResultCodes::Success,
            }
        };
        let state = state.on_mlme_event(&device_info, auth_conf, &mlme_sink, &user_sink);

        // (sme->mlme) Expect an AssociateRequest
        match mlme_stream.try_next().unwrap() {
            Some(MlmeRequest::Associate(req)) => {
                assert_eq!([ 7, 7, 7, 7, 7, 7 ], req.peer_sta_address);
            },
            other => panic!("expected an Associate request, got {:?}", other)
        }

        // (mlme->sme) Send an AssociateConf
        let assoc_conf = MlmeEvent::AssociateConf {
            resp: fidl_mlme::AssociateConfirm {
                result_code: fidl_mlme::AssociateResultCodes::Success,
                association_id: 55,
            }
        };
        let state = state.on_mlme_event(&device_info, assoc_conf, &mlme_sink, &user_sink);

        // We should now be in the associated state
        match state {
            State::Associated { bss, link_state, .. } => {
                assert_eq!([ 7, 7, 7, 7, 7, 7 ], bss.bssid);
                match link_state {
                    LinkState::LinkUp(None) => {},
                    other => panic!("Wrong link state {:?}", other)
                }
            },
            other => panic!("Expected an Associated state, got {:?}", other)
        }
    }

    fn bss(ssid: Ssid, bssid: [u8; 6]) -> fidl_mlme::BssDescription {
        fidl_mlme::BssDescription {
            bssid,
            .. fake_bss_description(ssid)
        }
    }

    fn fake_device_info() -> DeviceInfo {
        DeviceInfo {
            supported_channels: HashSet::from_iter([1, 2, 3].into_iter().cloned()),
            addr: [ 0, 1, 2, 3, 4, 5 ],
        }
    }

}
