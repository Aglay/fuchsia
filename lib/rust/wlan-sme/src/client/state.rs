// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use fidl_fuchsia_wlan_mlme::{self as fidl_mlme, BssDescription, MlmeEvent};
use log::{error, warn};
use wlan_rsn::key::exchange::Key;
use wlan_rsn::rsna::{self, SecAssocUpdate, SecAssocStatus};

use super::bss::convert_bss_description;
use super::{ConnectFailure, ConnectPhyParams, ConnectResult, InfoEvent, Status, Tokens};
use super::rsn::Rsna;

use crate::MlmeRequest;
use crate::client::{Context, event::{self, Event}, report_connect_finished};
use crate::clone_utils::clone_bss_desc;
use crate::phy_selection::{derive_phy_cbw};
use crate::sink::MlmeSink;
use crate::timer::EventId;

const DEFAULT_JOIN_FAILURE_TIMEOUT: u32 = 20; // beacon intervals
const DEFAULT_AUTH_FAILURE_TIMEOUT: u32 = 20; // beacon intervals

#[derive(Debug, PartialEq)]
pub enum LinkState<T: Tokens> {
    EstablishingRsna {
        token: Option<T::ConnectToken>,
        rsna: Rsna,
        // Timeout for the total duration RSNA may take to complete.
        rsna_timeout: Option<EventId>,
        // Timeout waiting to receive a key frame from the Authenticator. This timeout is None at
        // the beginning of the RSNA when no frame has been exchanged yet, or at the end of the
        // RSNA when all the key frames have finished exchanging.
        resp_timeout: Option<EventId>,
    },
    LinkUp(Option<Rsna>)
}

#[derive(Debug, PartialEq)]
pub struct ConnectCommand<T> {
    pub bss: Box<BssDescription>,
    pub token: Option<T>,
    pub rsna: Option<Rsna>,
    pub params: ConnectPhyParams
}

#[derive(Debug)]
pub enum RsnaStatus {
    Established,
    Failed(ConnectResult),
    Unchanged,
    Progressed {
        new_resp_timeout: Option<EventId>,
    },
}

#[derive(Debug, PartialEq)]
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
        params: ConnectPhyParams,
    },
}

impl<T: Tokens> State<T> {
    pub fn on_mlme_event(self, event: MlmeEvent, context: &mut Context<T>) -> Self {
        match self {
            State::Idle => {
                warn!("Unexpected MLME message while Idle: {:?}", event);
                State::Idle
            },
            State::Joining{ cmd } => match event {
                MlmeEvent::JoinConf { resp } => match resp.result_code {
                    fidl_mlme::JoinResultCodes::Success => {
                        context.mlme_sink.send(MlmeRequest::Authenticate(
                            fidl_mlme::AuthenticateRequest {
                                peer_sta_address: cmd.bss.bssid.clone(),
                                auth_type: fidl_mlme::AuthenticationTypes::OpenSystem,
                                auth_failure_timeout: DEFAULT_AUTH_FAILURE_TIMEOUT,
                            }));
                        State::Authenticating { cmd }
                    },
                    other => {
                        error!("Join request failed with result code {:?}", other);
                        report_connect_finished(cmd.token, &context, ConnectResult::Failed,
                                                Some(ConnectFailure::JoinFailure(other)),
                        );
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
                        to_associating_state(cmd, &context.mlme_sink)
                    },
                    other => {
                        error!("Authenticate request failed with result code {:?}", other);
                        report_connect_finished(cmd.token, &context, ConnectResult::Failed,
                                                Some(ConnectFailure::AuthenticationFailure(other)),
                        );
                        State::Idle
                    }
                },
                _ => State::Authenticating{ cmd }
            },
            State::Associating{ cmd } => match event {
                MlmeEvent::AssociateConf { resp } => match resp.result_code {
                    fidl_mlme::AssociateResultCodes::Success => {
                        context.info_sink.send(
                            InfoEvent::AssociationSuccess { att_id: context.att_id });
                        match cmd.rsna {
                            Some(mut rsna) => match rsna.supplicant.start() {
                                Err(e) => {
                                    handle_supplicant_start_failure(cmd.token, cmd.bss,
                                                                    &context, e);
                                    State::Idle
                                },
                                Ok(_) => {
                                    context.info_sink.send(
                                        InfoEvent::RsnaStarted { att_id: context.att_id });

                                    let rsna_timeout = Some(context.timer.schedule(
                                        Event::EstablishingRsnaTimeout));
                                    State::Associated {
                                        bss: cmd.bss,
                                        last_rssi: None,
                                        link_state: LinkState::EstablishingRsna {
                                            token: cmd.token,
                                            rsna,
                                            rsna_timeout,
                                            resp_timeout: None,
                                        },
                                        params: cmd.params,
                                    }
                                }
                            },
                            None => {
                                report_connect_finished(cmd.token, &context,
                                                        ConnectResult::Success, None);
                                State::Associated {
                                    bss: cmd.bss,
                                    last_rssi: None,
                                    link_state: LinkState::LinkUp(None),
                                    params: cmd.params,
                                }
                            }
                        }
                    },
                    other => {
                        error!("Associate request failed with result code {:?}", other);
                        report_connect_finished(cmd.token, &context, ConnectResult::Failed,
                                                Some(ConnectFailure::AssociationFailure(other)),
                        );
                        State::Idle
                    }
                },
                _ => State::Associating{ cmd }
            },
            State::Associated { bss, last_rssi, link_state, params, } => match event {
                MlmeEvent::DisassociateInd{ .. } => {
                    let (token, mut rsna) = match link_state {
                        LinkState::LinkUp(rsna) => (None, rsna),
                        LinkState::EstablishingRsna{ token, rsna, .. } => (token, Some(rsna)),
                    };
                    // Client is disassociating. The ESS-SA must be kept alive but reset.
                    if let Some(rsna) = &mut rsna {
                        rsna.supplicant.reset();
                    }

                    let cmd = ConnectCommand{
                        bss,
                        token,
                        rsna,
                        params,
                    };
                    context.att_id += 1;
                    to_associating_state(cmd, &context.mlme_sink)
                },
                MlmeEvent::DeauthenticateInd{ ind } => {
                    if let LinkState::EstablishingRsna{ token, .. } = link_state {
                        let connect_result = deauth_code_to_connect_result(ind.reason_code);
                        report_connect_finished(token, &context, connect_result, None);
                    }
                    State::Idle
                },
                MlmeEvent::SignalReport{ ind } => {
                    State::Associated {
                        bss,
                        last_rssi: Some(ind.rssi_dbm),
                        link_state,
                        params,
                    }
                },
                MlmeEvent::EapolInd{ ref ind } if bss.rsn.is_some() => match link_state {
                    LinkState::EstablishingRsna{ token, mut rsna, rsna_timeout,
                                                 mut resp_timeout } => {
                        match process_eapol_ind(context, &mut rsna, &ind) {
                            RsnaStatus::Established => {
                                context.mlme_sink.send(MlmeRequest::SetCtrlPort(
                                    fidl_mlme::SetControlledPortRequest {
                                        peer_sta_address: bss.bssid.clone(),
                                        state: fidl_mlme::ControlledPortState::Open,
                                    }
                                ));
                                context.info_sink.send(
                                    InfoEvent::RsnaEstablished { att_id: context.att_id });
                                report_connect_finished(token, &context,
                                                        ConnectResult::Success, None);
                                let link_state = LinkState::LinkUp(Some(rsna));
                                State::Associated { bss, last_rssi, link_state, params, }
                            },
                            RsnaStatus::Failed(result) => {
                                report_connect_finished(token, &context, result, None);
                                send_deauthenticate_request(bss, &context.mlme_sink);
                                State::Idle
                            },
                            RsnaStatus::Unchanged => {
                                let link_state = LinkState::EstablishingRsna {
                                    token, rsna, rsna_timeout, resp_timeout };
                                State::Associated { bss, last_rssi, link_state, params, }
                            },
                            RsnaStatus::Progressed { new_resp_timeout } => {
                                cancel(&mut resp_timeout);
                                if let Some(id) = new_resp_timeout {
                                    resp_timeout.replace(id);
                                }
                                let link_state = LinkState::EstablishingRsna {
                                    token, rsna, rsna_timeout, resp_timeout};
                                State::Associated { bss, last_rssi, link_state, params, }
                            }
                        }
                    },
                    LinkState::LinkUp(Some(mut rsna)) => {
                        match process_eapol_ind(context, &mut rsna, &ind) {
                            RsnaStatus::Unchanged => {},
                            // Once re-keying is supported, the RSNA can fail in LinkUp as well
                            // and cause deauthentication.
                            s => error!("unexpected RsnaStatus in LinkUp state: {:?}", s),
                        };
                        let link_state = LinkState::LinkUp(Some(rsna));
                        State::Associated { bss, last_rssi, link_state, params, }
                    },
                    _ => panic!("expected Link to carry RSNA because bss.rsn is present"),
                }
                _ => State::Associated{ bss, last_rssi, link_state, params, }
            },
        }
    }

    pub fn handle_timeout(self, event_id: EventId, event: Event, context: &mut Context<T>) -> Self {
        match self {
            State::Associated { bss, last_rssi, link_state, params } => match link_state {
                LinkState::EstablishingRsna { token, rsna, mut rsna_timeout, mut resp_timeout } => {
                    match event {
                        Event::EstablishingRsnaTimeout if triggered(&rsna_timeout,
                                                                    event_id) => {
                            error!("timeout establishing RSNA; deauthenticating");
                            cancel(&mut rsna_timeout);
                            report_connect_finished(token, &context, ConnectResult::Failed,
                                                    Some(ConnectFailure::RsnaTimeout));
                            send_deauthenticate_request(bss, &context.mlme_sink);
                            State::Idle
                        },
                        Event::KeyFrameExchangeTimeout { bssid, sta_addr, frame, attempt } => {
                            if !triggered(&resp_timeout, event_id) {
                                let link_state = LinkState::EstablishingRsna {
                                    token, rsna, rsna_timeout, resp_timeout, };
                                return State::Associated { bss, last_rssi, link_state, params }
                            }

                            if attempt < event::KEY_FRAME_EXCHANGE_MAX_ATTEMPTS {
                                warn!("timeout waiting for key frame for attempt {}; retrying",
                                      attempt);
                                let id = send_eapol_frame(context, bssid, sta_addr, frame,
                                                          attempt + 1);
                                resp_timeout.replace(id);
                                let link_state = LinkState::EstablishingRsna {
                                    token, rsna, rsna_timeout, resp_timeout, };
                                State::Associated { bss, last_rssi, link_state, params }
                            } else {
                                error!("timeout waiting for key frame for last attempt; deauth");
                                cancel(&mut resp_timeout);
                                report_connect_finished(token, &context, ConnectResult::Failed,
                                                        Some(ConnectFailure::RsnaTimeout));
                                send_deauthenticate_request(bss, &context.mlme_sink);
                                State::Idle
                            }
                        },
                        _ => {
                            let link_state = LinkState::EstablishingRsna {
                                token, rsna, rsna_timeout, resp_timeout };
                            State::Associated { bss, last_rssi, link_state, params }
                        },
                    }
                },
                _ => State::Associated { bss, last_rssi, link_state, params },
            },
            _ => self,
        }
    }

    pub fn connect(self, cmd: ConnectCommand<T::ConnectToken>, context: &mut Context<T>) -> Self {
        self.disconnect_internal(context);

        let mut selected_bss = clone_bss_desc(&cmd.bss);
        let (phy_to_use, cbw_to_use)
            = derive_phy_cbw(&selected_bss, &context.device_info, &cmd.params);
        selected_bss.chan.cbw = cbw_to_use;

        context.mlme_sink.send(MlmeRequest::Join(
            fidl_mlme::JoinRequest {
                selected_bss,
                join_failure_timeout: DEFAULT_JOIN_FAILURE_TIMEOUT,
                nav_sync_delay: 0,
                op_rate_set: vec![],
                phy: phy_to_use,
                cbw: cbw_to_use,
            }
        ));
        context.att_id += 1;
        context.info_sink.send(InfoEvent::AssociationStarted { att_id: context.att_id });
        State::Joining { cmd }
    }

    pub fn disconnect(self, context: &Context<T>) -> Self {
        self.disconnect_internal(context);
        State::Idle
    }

    fn disconnect_internal(self, context: &Context<T>) {
        match self {
            State::Idle => {},
            State::Joining { cmd } | State::Authenticating { cmd }  => {
                report_connect_finished(cmd.token, &context, ConnectResult::Canceled, None);
            },
            State::Associating{ cmd, .. } => {
                report_connect_finished(cmd.token, &context, ConnectResult::Canceled, None);
                send_deauthenticate_request(cmd.bss, &context.mlme_sink);
            },
            State::Associated { bss, .. } => {
                send_deauthenticate_request(bss, &context.mlme_sink);
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
                    connecting_to: Some(cmd.bss.ssid.clone()),
                }
            },
            State::Associated { bss, link_state: LinkState::EstablishingRsna { .. }, .. } => {
                Status {
                    connected_to: None,
                    connecting_to: Some(bss.ssid.clone()),
                }
            },
            State::Associated { bss, link_state: LinkState::LinkUp(..), .. } => Status {
                connected_to: Some(convert_bss_description(bss)),
                connecting_to: None,
            },
        }
    }
}

fn triggered(id: &Option<EventId>, received_id: EventId) -> bool {
    id.map_or(false, |id| id == received_id)
}

fn cancel(event_id: &mut Option<EventId>) {
    let _ = event_id.take();
}


fn deauth_code_to_connect_result(reason_code: fidl_mlme::ReasonCode) -> ConnectResult {
    match reason_code {
        fidl_mlme::ReasonCode::InvalidAuthentication
        | fidl_mlme::ReasonCode::Ieee8021XAuthFailed => ConnectResult::BadCredentials,
        _ => ConnectResult::Failed
    }
}

fn process_eapol_ind<T: Tokens>(context: &mut Context<T>, rsna: &mut Rsna,
                                ind: &fidl_mlme::EapolIndication)
                                -> RsnaStatus
{
    let mic_size = rsna.negotiated_rsne.mic_size;
    let eapol_pdu = &ind.data[..];
    let eapol_frame = match eapol::key_frame_from_bytes(eapol_pdu, mic_size).to_full_result() {
        Ok(key_frame) => eapol::Frame::Key(key_frame),
        Err(e) => {
            error!("received invalid EAPOL Key frame: {:?}", e);
            return RsnaStatus::Unchanged;
        }
    };

    let mut update_sink = rsna::UpdateSink::default();
    match rsna.supplicant.on_eapol_frame(&mut update_sink, &eapol_frame) {
        Err(e) => {
            error!("error processing EAPOL key frame: {}", e);
            return RsnaStatus::Unchanged;
        }
        Ok(_) if update_sink.is_empty() => return RsnaStatus::Unchanged,
        _ => (),
    }

    let bssid = ind.src_addr;
    let sta_addr = ind.dst_addr;
    let mut new_resp_timeout = None;
    for update in update_sink {
        match update {
            // ESS Security Association requests to send an EAPOL frame.
            // Forward EAPOL frame to MLME.
            SecAssocUpdate::TxEapolKeyFrame(frame) => {
                new_resp_timeout.replace(send_eapol_frame(context, bssid, sta_addr, frame, 1));
            },
            // ESS Security Association derived a new key.
            // Configure key in MLME.
            SecAssocUpdate::Key(key) => {
                send_keys(&context.mlme_sink, bssid, key)
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
                // TODO(hahnr): The API should not expose whether or not the connection failed
                // because of bad credentials as it allows callers to reason about location
                // information since the network was apparently found.
                SecAssocStatus::WrongPassword => {
                    return RsnaStatus::Failed(ConnectResult::BadCredentials);
                }
            },
        }
    }

    RsnaStatus::Progressed { new_resp_timeout }
}

fn send_eapol_frame<T: Tokens>(context: &mut Context<T>, bssid: [u8; 6], sta_addr: [u8; 6],
                               frame: eapol::KeyFrame, attempt: u32)
                               -> EventId
{
    let resp_timeout_id = context.timer.schedule(Event::KeyFrameExchangeTimeout {
        bssid,
        sta_addr,
        frame: frame.clone(),
        attempt
    });

    let mut buf = Vec::with_capacity(frame.len());
    frame.as_bytes(false, &mut buf);
    context.mlme_sink.send(MlmeRequest::Eapol(
        fidl_mlme::EapolRequest {
            src_addr: sta_addr,
            dst_addr: bssid,
            data: buf,
        }
    ));
    resp_timeout_id
}

fn send_keys(mlme_sink: &MlmeSink, bssid: [u8; 6], key: Key)
{
    match key {
        Key::Ptk(ptk) => {
            mlme_sink.send(MlmeRequest::SetKeys(
                fidl_mlme::SetKeysRequest {
                    keylist: vec![fidl_mlme::SetKeyDescriptor{
                        key_type: fidl_mlme::KeyType::Pairwise,
                        key: ptk.tk().to_vec(),
                        key_id: 0,
                        address: bssid,
                        cipher_suite_oui: eapol::to_array(&ptk.cipher.oui[..]),
                        cipher_suite_type: ptk.cipher.suite_type,
                        rsc: [0u8; 8],
                    }]
                }
            ));
        },
        Key::Gtk(gtk) => {
            mlme_sink.send(MlmeRequest::SetKeys(
                fidl_mlme::SetKeysRequest {
                    keylist: vec![fidl_mlme::SetKeyDescriptor{
                        key_type: fidl_mlme::KeyType::Group,
                        key: gtk.tk().to_vec(),
                        key_id: gtk.key_id() as u16,
                        address: [0xFFu8; 6],
                        cipher_suite_oui: eapol::to_array(&gtk.cipher.oui[..]),
                        cipher_suite_type: gtk.cipher.suite_type,
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
        let s_rsne = rsna.negotiated_rsne.to_full_rsne();
        let mut buf = Vec::with_capacity(s_rsne.len());
        s_rsne.as_bytes(&mut buf);
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

fn handle_supplicant_start_failure<T>(token: Option<T::ConnectToken>, bss: Box<BssDescription>,
                                      context: &Context<T>, e: failure::Error) where T: Tokens
{
    error!("deauthenticating; could not start Supplicant: {}", e);
    send_deauthenticate_request(bss, &context.mlme_sink);

    // TODO(hahnr): Report RSNA specific failure instead.
    let reason = fidl_mlme::AssociateResultCodes::RefusedReasonUnspecified;
    report_connect_finished(token, &context,
                            ConnectResult::Failed,
                            Some(ConnectFailure::AssociationFailure(reason)));
}

#[cfg(test)]
mod tests {
    use super::*;
    use failure::format_err;
    use futures::channel::mpsc;
    use std::error::Error;
    use std::sync::Arc;
    use wlan_rsn::{NegotiatedRsne, rsna::UpdateSink, rsne::RsnCapabilities};

    use crate::client::test_utils::{
        expect_info_event,
        fake_protected_bss_description,
        fake_unprotected_bss_description,
        mock_supplicant,
        MockSupplicant,
        MockSupplicantController,

    };
    use crate::client::{InfoSink, TimeStream, UserEvent, UserStream, UserSink};
    use crate::{DeviceInfo, InfoStream, MlmeStream, Ssid, test_utils, timer};

    #[derive(Debug, PartialEq)]
    struct FakeTokens;

    impl Tokens for FakeTokens {
        type ScanToken = u32;
        type ConnectToken = u32;
    }

    #[test]
    fn associate_happy_path_unprotected() {
        let mut h = TestHelper::new();

        let state = idle_state();
        let command = connect_command_one();
        let bss_ssid = command.bss.ssid.clone();
        let bssid = command.bss.bssid.clone();
        let command_token = command.token.unwrap();

        // Issue a "connect" command
        let state = state.connect(command, &mut h.context);

        expect_info_event(&mut h.info_stream, InfoEvent::AssociationStarted { att_id: 1});
        expect_join_request(&mut h.mlme_stream, &bss_ssid);

        // (mlme->sme) Send a JoinConf as a response
        let join_conf = create_join_conf(fidl_mlme::JoinResultCodes::Success);
        let state = state.on_mlme_event(join_conf, &mut h.context);

        expect_auth_req(&mut h.mlme_stream, bssid);

        // (mlme->sme) Send an AuthenticateConf as a response
        let auth_conf = create_auth_conf(bssid.clone(),
                                         fidl_mlme::AuthenticateResultCodes::Success);
        let state = state.on_mlme_event(auth_conf, &mut h.context);

        expect_assoc_req(&mut h.mlme_stream, bssid);

        // (mlme->sme) Send an AssociateConf
        let assoc_conf = create_assoc_conf(fidl_mlme::AssociateResultCodes::Success);
        let _state = state.on_mlme_event(assoc_conf, &mut h.context);

        // User should be notified that we are connected
        expect_connect_result(&mut h.user_stream, command_token, ConnectResult::Success);

        expect_info_event(&mut h.info_stream, InfoEvent::AssociationSuccess { att_id: 1 });
        expect_info_event(
            &mut h.info_stream,
            InfoEvent::ConnectFinished { result: ConnectResult::Success, failure: None });
    }

    #[test]
    fn associate_happy_path_protected() {
        let mut h = TestHelper::new();
        let (supplicant, suppl_mock) = mock_supplicant();

        let state = idle_state();
        let command = connect_command_rsna(supplicant);
        let bss_ssid = command.bss.ssid.clone();
        let bssid = command.bss.bssid.clone();
        let command_token = command.token.unwrap();

        // Issue a "connect" command
        let state = state.connect(command, &mut h.context);

        expect_info_event(&mut h.info_stream, InfoEvent::AssociationStarted { att_id: 1});
        expect_join_request(&mut h.mlme_stream, &bss_ssid);

        // (mlme->sme) Send a JoinConf as a response
        let join_conf = create_join_conf(fidl_mlme::JoinResultCodes::Success);
        let state = state.on_mlme_event(join_conf, &mut h.context);

        expect_auth_req(&mut h.mlme_stream, bssid);

        // (mlme->sme) Send an AuthenticateConf as a response
        let auth_conf = create_auth_conf(bssid.clone(),
                                         fidl_mlme::AuthenticateResultCodes::Success);
        let state = state.on_mlme_event(auth_conf, &mut h.context);

        expect_assoc_req(&mut h.mlme_stream, bssid);

        // (mlme->sme) Send an AssociateConf
        let assoc_conf = create_assoc_conf(fidl_mlme::AssociateResultCodes::Success);
        let state = state.on_mlme_event(assoc_conf, &mut h.context);

        assert!(suppl_mock.is_supplicant_started());
        expect_info_event(&mut h.info_stream, InfoEvent::AssociationSuccess { att_id: 1 });
        expect_info_event(&mut h.info_stream, InfoEvent::RsnaStarted { att_id: 1 });

        // (mlme->sme) Send an EapolInd, mock supplicant with key frame
        let update = SecAssocUpdate::TxEapolKeyFrame(test_utils::eapol_key_frame());
        let state = on_eapol_ind(state, &mut h, bssid, &suppl_mock, vec![update]);

        expect_eapol_req(&mut h.mlme_stream, bssid);

        // (mlme->sme) Send an EapolInd, mock supplicant with keys
        let ptk = SecAssocUpdate::Key(Key::Ptk(test_utils::ptk()));
        let gtk = SecAssocUpdate::Key(Key::Gtk(test_utils::gtk()));
        let state = on_eapol_ind(state, &mut h, bssid, &suppl_mock, vec![ptk, gtk]);

        expect_set_keys(&mut h.mlme_stream, bssid);

        // (mlme->sme) Send an EapolInd, mock supplicant with completion status
        let update = SecAssocUpdate::Status(SecAssocStatus::EssSaEstablished);
        let _state = on_eapol_ind(state, &mut h, bssid, &suppl_mock, vec![update]);

        expect_set_ctrl_port(&mut h.mlme_stream, bssid, fidl_mlme::ControlledPortState::Open);
        expect_connect_result(&mut h.user_stream, command_token, ConnectResult::Success);
        expect_info_event(&mut h.info_stream, InfoEvent::RsnaEstablished { att_id: 1 });
        expect_info_event(
            &mut h.info_stream,
            InfoEvent::ConnectFinished { result: ConnectResult::Success, failure: None });
    }

    #[test]
    fn join_failure() {
        let mut h = TestHelper::new();

        // Start in a "Joining" state
        let state = State::Joining::<FakeTokens> { cmd: connect_command_one() };

        // (mlme->sme) Send an unsuccessful JoinConf
        let join_conf = MlmeEvent::JoinConf {
            resp: fidl_mlme::JoinConfirm {
                result_code: fidl_mlme::JoinResultCodes::JoinFailureTimeout
            }
        };
        let state = state.on_mlme_event(join_conf, &mut h.context);
        assert_eq!(idle_state(), state);

        // User should be notified that connection attempt failed
        expect_connect_result(
            &mut h.user_stream,
            connect_command_one().token.unwrap(),
            ConnectResult::Failed,
        );

        expect_info_event(
            &mut h.info_stream,
            InfoEvent::ConnectFinished {
                result: ConnectResult::Failed,
                failure: Some(ConnectFailure::JoinFailure(
                    fidl_mlme::JoinResultCodes::JoinFailureTimeout,
                )),
            },
        );
    }

    #[test]
    fn authenticate_failure() {
        let mut h = TestHelper::new();

        // Start in an "Authenticating" state
        let state = State::Authenticating::<FakeTokens> { cmd: connect_command_one() };

        // (mlme->sme) Send an unsuccessful AuthenticateConf
        let auth_conf = MlmeEvent::AuthenticateConf {
            resp: fidl_mlme::AuthenticateConfirm {
                peer_sta_address: connect_command_one().bss.bssid,
                auth_type: fidl_mlme::AuthenticationTypes::OpenSystem,
                result_code: fidl_mlme::AuthenticateResultCodes::Refused,
            }
        };
        let state = state.on_mlme_event(auth_conf, &mut h.context);
        assert_eq!(idle_state(), state);

        // User should be notified that connection attempt failed
        expect_connect_result(
            &mut h.user_stream,
            connect_command_one().token.unwrap(),
            ConnectResult::Failed,
        );

        expect_info_event(
            &mut h.info_stream,
            InfoEvent::ConnectFinished {
                result: ConnectResult::Failed,
                failure: Some(ConnectFailure::AuthenticationFailure(
                    fidl_mlme::AuthenticateResultCodes::Refused,
                )),
            },
        );
    }

    #[test]
    fn associate_failure() {
        let mut h = TestHelper::new();

        // Start in an "Associating" state
        let state = State::Associating::<FakeTokens> { cmd: connect_command_one() };

        // (mlme->sme) Send an unsuccessful AssociateConf
        let assoc_conf = create_assoc_conf(
            fidl_mlme::AssociateResultCodes::RefusedReasonUnspecified);
        let state = state.on_mlme_event(assoc_conf, &mut h.context);
        assert_eq!(idle_state(), state);

        // User should be notified that connection attempt failed
        expect_connect_result(
            &mut h.user_stream,
            connect_command_one().token.unwrap(),
            ConnectResult::Failed,
        );

        expect_info_event(
            &mut h.info_stream,
            InfoEvent::ConnectFinished {
                result: ConnectResult::Failed,
                failure: Some(ConnectFailure::AssociationFailure(
                    fidl_mlme::AssociateResultCodes::RefusedReasonUnspecified,
                )),
            },
        );
    }

    #[test]
    fn connect_while_joining() {
        let mut h = TestHelper::new();
        let state = joining_state(connect_command_one());
        let state = state.connect(connect_command_two(), &mut h.context);
        expect_connect_result(&mut h.user_stream, connect_command_one().token.unwrap(),
                              ConnectResult::Canceled);
        expect_join_request(&mut h.mlme_stream, &connect_command_two().bss.ssid);
        assert_eq!(joining_state(connect_command_two()), state);
    }

    #[test]
    fn connect_while_authenticating() {
        let mut h = TestHelper::new();
        let state = authenticating_state(connect_command_one());
        let state = state.connect(connect_command_two(), &mut h.context);
        expect_connect_result(&mut h.user_stream, connect_command_one().token.unwrap(),
                              ConnectResult::Canceled);
        expect_join_request(&mut h.mlme_stream, &connect_command_two().bss.ssid);
        assert_eq!(joining_state(connect_command_two()), state);
    }

    #[test]
    fn connect_while_associating() {
        let mut h = TestHelper::new();
        let state = associating_state(connect_command_one());
        let state = state.connect(connect_command_two(), &mut h.context);
        let state = exchange_deauth(state, &mut h);
        expect_connect_result(&mut h.user_stream, connect_command_one().token.unwrap(),
                              ConnectResult::Canceled);
        expect_join_request(&mut h.mlme_stream, &connect_command_two().bss.ssid);
        assert_eq!(joining_state(connect_command_two()), state);
    }

    #[test]
    fn supplicant_fails_to_start_while_associating() {
        let mut h = TestHelper::new();
        let (supplicant, suppl_mock) = mock_supplicant();
        let command = connect_command_rsna(supplicant);
        let bssid = command.bss.bssid.clone();
        let token = command.token.unwrap();
        let state = associating_state(command);

        suppl_mock.set_start_failure(format_err!("failed to start supplicant"));

        // (mlme->sme) Send an AssociateConf
        let assoc_conf = create_assoc_conf(fidl_mlme::AssociateResultCodes::Success);
        let _state = state.on_mlme_event(assoc_conf, &mut h.context);

        expect_deauth_req(&mut h.mlme_stream, bssid, fidl_mlme::ReasonCode::StaLeaving);
        expect_connect_result(&mut h.user_stream, token, ConnectResult::Failed);
        expect_info_event(&mut h.info_stream, InfoEvent::AssociationSuccess { att_id: 0 });
        expect_info_event(
            &mut h.info_stream,
            InfoEvent::ConnectFinished {
                result: ConnectResult::Failed,
                failure: Some(ConnectFailure::AssociationFailure(
                    fidl_mlme::AssociateResultCodes::RefusedReasonUnspecified,
                )),
            },
        );
    }

    #[test]
    fn bad_eapol_frame_while_establishing_rsna() {
        let mut h = TestHelper::new();
        let (supplicant, suppl_mock) = mock_supplicant();
        let command = connect_command_rsna(supplicant);
        let bssid = command.bss.bssid.clone();
        let state = establishing_rsna_state(command);

        // doesn't matter what we mock here
        let update = SecAssocUpdate::Status(SecAssocStatus::EssSaEstablished);
        suppl_mock.set_on_eapol_frame_results(vec![update]);

        // (mlme->sme) Send an EapolInd with bad eapol data
        let eapol_ind = create_eapol_ind(bssid.clone(), vec![1, 2, 3, 4]);
        let state = state.on_mlme_event(eapol_ind, &mut h.context);

        match state {
            State::Associated { link_state, .. } => match link_state {
                LinkState::EstablishingRsna { .. } => (), // expected path
                _ => panic!("expect link state to still be establishing RSNA"),
            },
            _ => panic!("expect state to still be associated"),
        }

        expect_stream_empty(&mut h.mlme_stream, "unexpected event in mlme stream");
        expect_stream_empty(&mut h.user_stream, "unexpected event in user stream");
        expect_stream_empty(&mut h.info_stream, "unexpected event in info stream");
    }

    #[test]
    fn supplicant_fails_to_process_eapol_while_establishing_rsna() {
        let mut h = TestHelper::new();
        let (supplicant, suppl_mock) = mock_supplicant();
        let command = connect_command_rsna(supplicant);
        let bssid = command.bss.bssid.clone();
        let state = establishing_rsna_state(command);

        suppl_mock.set_on_eapol_frame_failure(format_err!("supplicant::on_eapol_frame fails"));

        // (mlme->sme) Send an EapolInd
        let eapol_ind = create_eapol_ind(bssid.clone(), test_utils::eapol_key_frame_bytes());
        let state = state.on_mlme_event(eapol_ind, &mut h.context);

        match state {
            State::Associated { link_state, .. } => match link_state {
                LinkState::EstablishingRsna { .. } => (), // expected path
                _ => panic!("expect link state to still be establishing RSNA"),
            },
            _ => panic!("expect state to still be associated"),
        }

        expect_stream_empty(&mut h.mlme_stream, "unexpected event in mlme stream");
        expect_stream_empty(&mut h.user_stream, "unexpected event in user stream");
        expect_stream_empty(&mut h.info_stream, "unexpected event in info stream");
    }

    #[test]
    fn wrong_password_while_establishing_rsna() {
        let mut h = TestHelper::new();
        let (supplicant, suppl_mock) = mock_supplicant();
        let command = connect_command_rsna(supplicant);
        let bssid = command.bss.bssid.clone();
        let token = command.token.unwrap();
        let state = establishing_rsna_state(command);

        // (mlme->sme) Send an EapolInd, mock supplicant with wrong password status
        let update = SecAssocUpdate::Status(SecAssocStatus::WrongPassword);
        let _state = on_eapol_ind(state, &mut h, bssid, &suppl_mock, vec![update]);

        expect_deauth_req(&mut h.mlme_stream, bssid, fidl_mlme::ReasonCode::StaLeaving);
        expect_connect_result(&mut h.user_stream, token, ConnectResult::BadCredentials);
        expect_info_event(
            &mut h.info_stream,
            InfoEvent::ConnectFinished {
                result: ConnectResult::BadCredentials,
                failure: None,
            },
        );
    }

    #[test]
    fn overall_timeout_while_establishing_rsna() {
        let mut h = TestHelper::new();
        let (supplicant, _suppl_mock) = mock_supplicant();
        let command = connect_command_rsna(supplicant);
        let bssid = command.bss.bssid.clone();
        let token = command.token.unwrap();

        // Start in an "Associating" state
        let state = State::Associating::<FakeTokens> { cmd: command };
        let assoc_conf = create_assoc_conf(fidl_mlme::AssociateResultCodes::Success);
        let state = state.on_mlme_event(assoc_conf, &mut h.context);

        let (_, timed_event) = h.time_stream.try_next().unwrap().expect("expect timed event");
        match timed_event.event {
            Event::EstablishingRsnaTimeout => (), // expected path
            _ => panic!("expect EstablishingRsnaTimeout timeout event"),
        }

        expect_stream_empty(&mut h.mlme_stream, "unexpected event in mlme stream");

        let _state = state.handle_timeout(timed_event.id, timed_event.event, &mut h.context);

        expect_deauth_req(&mut h.mlme_stream, bssid, fidl_mlme::ReasonCode::StaLeaving);
        expect_connect_result(&mut h.user_stream, token, ConnectResult::Failed);
    }

    #[test]
    fn key_frame_exchange_timeout_while_establishing_rsna() {
        let mut h = TestHelper::new();
        let (supplicant, suppl_mock) = mock_supplicant();
        let command = connect_command_rsna(supplicant);
        let bssid = command.bss.bssid.clone();
        let token = command.token.unwrap();
        let state = establishing_rsna_state(command);

        // (mlme->sme) Send an EapolInd, mock supplication with key frame
        let update = SecAssocUpdate::TxEapolKeyFrame(test_utils::eapol_key_frame());
        let mut state = on_eapol_ind(state, &mut h, bssid, &suppl_mock, vec![update]);

        for i in 1..=3 {
            println!("send eapol attempt: {}", i);
            expect_eapol_req(&mut h.mlme_stream, bssid);
            expect_stream_empty(&mut h.mlme_stream, "unexpected event in mlme stream");

            let (_, timed_event) = h.time_stream.try_next().unwrap().expect("expect timed event");
            match timed_event.event {
                Event::KeyFrameExchangeTimeout { attempt, .. } => assert_eq!(attempt, i),
                _ => panic!("expect EstablishingRsnaTimeout timeout event"),
            }
            state = state.handle_timeout(timed_event.id, timed_event.event, &mut h.context);
        }

        expect_deauth_req(&mut h.mlme_stream, bssid, fidl_mlme::ReasonCode::StaLeaving);
        expect_connect_result(&mut h.user_stream, token, ConnectResult::Failed);
    }


    #[test]
    fn connect_while_link_up() {
        let mut h = TestHelper::new();
        let state = link_up_state(connect_command_one().bss);
        let state = state.connect(connect_command_two(), &mut h.context);
        let state = exchange_deauth(state, &mut h);
        expect_join_request(&mut h.mlme_stream, &connect_command_two().bss.ssid);
        assert_eq!(joining_state(connect_command_two()), state);
    }

    #[test]
    fn disconnect_while_idle() {
        let mut h = TestHelper::new();
        let new_state = idle_state().disconnect(&h.context);
        assert_eq!(idle_state(), new_state);
        // Expect no messages to the MLME or the user
        assert!(h.mlme_stream.try_next().is_err());
        assert!(h.user_stream.try_next().is_err());
    }

    #[test]
    fn disconnect_while_joining() {
        let mut h = TestHelper::new();
        let state = joining_state(connect_command_one());
        let state = state.disconnect(&h.context);
        expect_connect_result(&mut h.user_stream, connect_command_one().token.unwrap(),
                              ConnectResult::Canceled);
        assert_eq!(idle_state(), state);
    }

    #[test]
    fn disconnect_while_authenticating() {
        let mut h = TestHelper::new();
        let state = authenticating_state(connect_command_one());
        let state = state.disconnect(&h.context);
        expect_connect_result(&mut h.user_stream, connect_command_one().token.unwrap(),
                              ConnectResult::Canceled);
        assert_eq!(idle_state(), state);
    }

    #[test]
    fn disconnect_while_associating() {
        let mut h = TestHelper::new();
        let state = associating_state(connect_command_one());
        let state = state.disconnect(&h.context);
        let state = exchange_deauth(state, &mut h);
        expect_connect_result(&mut h.user_stream, connect_command_one().token.unwrap(),
                              ConnectResult::Canceled);
        assert_eq!(idle_state(), state);
    }

    #[test]
    fn disconnect_while_link_up() {
        let mut h = TestHelper::new();
        let state = link_up_state(connect_command_one().bss);
        let state = state.disconnect(&h.context);
        let state = exchange_deauth(state, &mut h);
        assert_eq!(idle_state(), state);
    }

    #[test]
    fn increment_att_id_on_connect() {
        let mut h = TestHelper::new();
        let state = idle_state();
        assert_eq!(h.context.att_id, 0);

        let state = state.connect(connect_command_one(), &mut h.context);
        assert_eq!(h.context.att_id, 1);

        let state = state.disconnect(&h.context);
        assert_eq!(h.context.att_id, 1);

        let state = state.connect(connect_command_two(), &mut h.context);
        assert_eq!(h.context.att_id, 2);

        let _state = state.connect(connect_command_one(), &mut h.context);
        assert_eq!(h.context.att_id, 3);
    }

    #[test]
    fn increment_att_id_on_disassociate_ind() {
        let mut h = TestHelper::new();
        let state = link_up_state(connect_command_no_token().bss);
        assert_eq!(h.context.att_id, 0);

        let disassociate_ind = MlmeEvent::DisassociateInd {
            ind: fidl_mlme::DisassociateIndication {
                peer_sta_address: [0, 0, 0, 0, 0, 0],
                reason_code: 0,
            }
        };

        let state = state.on_mlme_event(disassociate_ind, &mut h.context);
        assert_eq!(associating_state(connect_command_no_token()), state);
        assert_eq!(h.context.att_id, 1);
    }

    struct TestHelper {
        mlme_stream: MlmeStream,
        user_stream: UserStream<FakeTokens>,
        info_stream: InfoStream,
        time_stream: TimeStream,
        context: Context<FakeTokens>,
    }

    impl TestHelper {
        fn new() -> Self {
            let (mlme_sink, mlme_stream) = mpsc::unbounded();
            let (user_sink, user_stream) = mpsc::unbounded();
            let (info_sink, info_stream) = mpsc::unbounded();
            let (timer, time_stream) = timer::create_timer();
            let context = Context {
                device_info: Arc::new(fake_device_info()),
                mlme_sink: MlmeSink::new(mlme_sink),
                user_sink: UserSink::new(user_sink),
                info_sink: InfoSink::new(info_sink),
                timer,
                att_id: 0,
            };
            TestHelper { mlme_stream, user_stream, info_stream, time_stream, context }
        }
    }

    fn on_eapol_ind(state: State<FakeTokens>, helper: &mut TestHelper, bssid: [u8; 6],
                    suppl_mock: &MockSupplicantController, update_sink: UpdateSink)
                    -> State<FakeTokens> {
        suppl_mock.set_on_eapol_frame_results(update_sink);
        // (mlme->sme) Send an EapolInd
        let eapol_ind = create_eapol_ind(bssid.clone(), test_utils::eapol_key_frame_bytes());
        state.on_mlme_event(eapol_ind, &mut helper.context)
    }

    fn create_join_conf(result_code: fidl_mlme::JoinResultCodes) -> MlmeEvent {
        MlmeEvent::JoinConf {
            resp: fidl_mlme::JoinConfirm {
                result_code,
            }
        }
    }

    fn create_auth_conf(bssid: [u8; 6], result_code: fidl_mlme::AuthenticateResultCodes)
                        -> MlmeEvent {
        MlmeEvent::AuthenticateConf {
            resp: fidl_mlme::AuthenticateConfirm {
                peer_sta_address: bssid,
                auth_type: fidl_mlme::AuthenticationTypes::OpenSystem,
                result_code,
            }
        }
    }

    fn create_assoc_conf(result_code: fidl_mlme::AssociateResultCodes) -> MlmeEvent {
        MlmeEvent::AssociateConf {
            resp: fidl_mlme::AssociateConfirm {
                result_code,
                association_id: 55,
            }
        }
    }

    fn create_eapol_ind(bssid: [u8; 6], data: Vec<u8>) -> MlmeEvent {
        MlmeEvent::EapolInd {
            ind: fidl_mlme::EapolIndication {
                src_addr: bssid,
                dst_addr: fake_device_info().addr,
                data,
            },
        }
    }

    fn exchange_deauth(state: State<FakeTokens>, h: &mut TestHelper) -> State<FakeTokens> {
        // (sme->mlme) Expect a DeauthenticateRequest
        match h.mlme_stream.try_next().unwrap() {
            Some(MlmeRequest::Deauthenticate(req)) => {
                assert_eq!(connect_command_one().bss.bssid, req.peer_sta_address);
            },
            other => panic!("expected a Deauthenticate request, got {:?}", other),
        }

        // (mlme->sme) Send a DeauthenticateConf as a response
        let deauth_conf = MlmeEvent::DeauthenticateConf {
            resp: fidl_mlme::DeauthenticateConfirm {
                peer_sta_address: connect_command_one().bss.bssid,
            }
        };
        state.on_mlme_event(deauth_conf, &mut h.context)
    }

    fn expect_join_request(mlme_stream: &mut MlmeStream, ssid: &[u8]) {
        // (sme->mlme) Expect a JoinRequest
        match mlme_stream.try_next().unwrap() {
            Some(MlmeRequest::Join(req)) => assert_eq!(ssid, &req.selected_bss.ssid[..]),
            _ => panic!("expect set keys req to MLME"),
        }
    }

    fn expect_set_ctrl_port(mlme_stream: &mut MlmeStream, bssid: [u8; 6],
                            state: fidl_mlme::ControlledPortState) {
        match mlme_stream.try_next().unwrap().expect("expect mlme message") {
            MlmeRequest::SetCtrlPort(req) => {
                assert_eq!(req.peer_sta_address, bssid);
                assert_eq!(req.state, state);
            },
            other => panic!("expected a Join request, got {:?}", other),
        }
    }

    fn expect_auth_req(mlme_stream: &mut MlmeStream, bssid: [u8; 6]) {
        // (sme->mlme) Expect an AuthenticateRequest
        match mlme_stream.try_next().unwrap() {
            Some(MlmeRequest::Authenticate(req)) => assert_eq!(bssid, req.peer_sta_address),
            other => panic!("expected an Authenticate request, got {:?}", other)
        }
    }

    fn expect_deauth_req(mlme_stream: &mut MlmeStream, bssid: [u8; 6],
                         reason_code: fidl_mlme::ReasonCode) {
        // (sme->mlme) Expect a DeauthenticateRequest
        match mlme_stream.try_next().unwrap() {
            Some(MlmeRequest::Deauthenticate(req)) => {
                assert_eq!(bssid, req.peer_sta_address);
                assert_eq!(reason_code, req.reason_code);
            },
            other => panic!("expected an Deauthenticate request, got {:?}", other)
        }
    }

    fn expect_assoc_req(mlme_stream: &mut MlmeStream, bssid: [u8; 6]) {
        match mlme_stream.try_next().unwrap() {
            Some(MlmeRequest::Associate(req)) => assert_eq!(bssid, req.peer_sta_address),
            other => panic!("expected an Associate request, got {:?}", other),
        }
    }

    fn expect_eapol_req(mlme_stream: &mut MlmeStream, bssid: [u8; 6]) {
        match mlme_stream.try_next().unwrap() {
            Some(MlmeRequest::Eapol(req)) => {
                assert_eq!(req.src_addr, fake_device_info().addr);
                assert_eq!(req.dst_addr, bssid);
                assert_eq!(req.data, test_utils::eapol_key_frame_bytes());
            }
            other => panic!("expected an Eapol request, got {:?}", other),
        }
    }

    fn expect_set_keys(mlme_stream: &mut MlmeStream, bssid: [u8; 6]) {
        match mlme_stream.try_next().unwrap().expect("expect mlme message") {
            MlmeRequest::SetKeys(set_keys_req) => {
                assert_eq!(set_keys_req.keylist.len(), 1);
                let k = set_keys_req.keylist.get(0).expect("expect key descriptor");
                assert_eq!(k.key, vec![0xCCu8; test_utils::cipher().tk_bytes().unwrap()]);
                assert_eq!(k.key_id, 0);
                assert_eq!(k.key_type, fidl_mlme::KeyType::Pairwise);
                assert_eq!(k.address, bssid);
                assert_eq!(k.rsc, [0u8; 8]);
                assert_eq!(k.cipher_suite_oui, [0x00, 0x0F, 0xAC]);
                assert_eq!(k.cipher_suite_type, 4);
            },
            _ => panic!("expect set keys req to MLME"),
        }

        match mlme_stream.try_next().unwrap().expect("expect mlme message") {
            MlmeRequest::SetKeys(set_keys_req) => {
                assert_eq!(set_keys_req.keylist.len(), 1);
                let k = set_keys_req.keylist.get(0).expect("expect key descriptor");
                assert_eq!(k.key, test_utils::gtk_bytes());
                assert_eq!(k.key_id, 2);
                assert_eq!(k.key_type, fidl_mlme::KeyType::Group);
                assert_eq!(k.address, [0xFFu8; 6]);
                assert_eq!(k.rsc, [0u8; 8]);
                assert_eq!(k.cipher_suite_oui, [0x00, 0x0F, 0xAC]);
                assert_eq!(k.cipher_suite_type, 4);
            },
            _ => panic!("expect set keys req to MLME"),
        }
    }

    fn expect_connect_result(user_stream: &mut UserStream<FakeTokens>,
                             expected_token: u32,
                             expected_result: ConnectResult) {
        match user_stream.try_next().unwrap() {
            Some(UserEvent::ConnectFinished { token, result }) => {
                assert_eq!(expected_token, token);
                assert_eq!(expected_result, result);
            },
            other => panic!("expected a ConnectFinished event, got {:?}", other)
        }
    }

    fn expect_stream_empty<T>(stream: &mut mpsc::UnboundedReceiver<T>, error_msg: &str) {
        match stream.try_next() {
            Err(e) => assert_eq!(e.description(), "receiver channel is empty"),
            _ => panic!("{}", error_msg),
        }
    }

    fn connect_command_one() -> ConnectCommand<u32> {
        ConnectCommand {
            bss: Box::new(unprotected_bss(b"foo".to_vec(), [7, 7, 7, 7, 7, 7])),
            token: Some(123_u32),
            rsna: None,
            params: ConnectPhyParams { phy: None, cbw: None, },
        }
    }

    fn connect_command_two() -> ConnectCommand<u32> {
        ConnectCommand {
            bss: Box::new(unprotected_bss(b"bar".to_vec(), [8, 8, 8, 8, 8, 8])),
            token: Some(456_u32),
            rsna: None,
            params: ConnectPhyParams { phy: None, cbw: None, },
        }
    }

    fn connect_command_no_token() -> ConnectCommand<u32> {
        ConnectCommand {
            bss: Box::new(unprotected_bss(b"bar".to_vec(), [8, 8, 8, 8, 8, 8])),
            token: None,
            rsna: None,
            params: ConnectPhyParams { phy: None, cbw: None, },
        }
    }

    fn connect_command_rsna(supplicant: MockSupplicant) -> ConnectCommand<u32> {
        let bss = protected_bss(b"foo".to_vec(), [7, 7, 7, 7, 7, 7]);
        let rsne = test_utils::wpa2_psk_ccmp_rsne_with_caps(RsnCapabilities(0));
        ConnectCommand {
            bss: Box::new(bss),
            token: Some(123_u32),
            rsna: Some(Rsna {
                negotiated_rsne: NegotiatedRsne::from_rsne(&rsne).expect("invalid NegotiatedRsne"),
                supplicant: Box::new(supplicant),
            }),
            params: ConnectPhyParams { phy: None, cbw: None, },
        }
    }

    fn idle_state() -> State<FakeTokens> {
        State::Idle
    }

    fn joining_state(cmd: ConnectCommand<u32>) -> State<FakeTokens> {
        State::Joining { cmd }
    }

    fn authenticating_state(cmd: ConnectCommand<u32>) -> State<FakeTokens> {
        State::Authenticating { cmd }
    }

    fn associating_state(cmd: ConnectCommand<u32>) -> State<FakeTokens> {
        State::Associating { cmd }
    }

    fn establishing_rsna_state(cmd: ConnectCommand<u32>) -> State<FakeTokens> {
        let rsna = cmd.rsna.expect("expect rsna for establishing_rsna_state");
        State::Associated {
            bss: cmd.bss,
            last_rssi: None,
            link_state: LinkState::EstablishingRsna {
                token: cmd.token,
                rsna,
                rsna_timeout: None,
                resp_timeout: None,
            },
            params: ConnectPhyParams { phy: None, cbw: None },
        }
    }

    fn link_up_state(bss: Box<fidl_mlme::BssDescription>) -> State<FakeTokens> {
        State::Associated {
            bss,
            last_rssi: None,
            link_state: LinkState::LinkUp(None),
            params: ConnectPhyParams { phy: None, cbw: None, },
        }
    }

    fn protected_bss(ssid: Ssid, bssid: [u8; 6]) -> fidl_mlme::BssDescription {
        fidl_mlme::BssDescription {
            bssid,
            .. fake_protected_bss_description(ssid)
        }
    }

    fn unprotected_bss(ssid: Ssid, bssid: [u8; 6]) -> fidl_mlme::BssDescription {
        fidl_mlme::BssDescription {
            bssid,
            .. fake_unprotected_bss_description(ssid)
        }
    }

    fn fake_device_info() -> DeviceInfo {
        DeviceInfo {
            addr: [ 0, 1, 2, 3, 4, 5 ],
            bands: vec![],
        }
    }
}
