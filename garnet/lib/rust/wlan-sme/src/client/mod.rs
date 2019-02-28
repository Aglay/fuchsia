// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

mod bss;
mod event;
mod rsn;
mod scan;
mod state;

#[cfg(test)]
pub mod test_utils;

use fidl_fuchsia_wlan_common as fidl_common;
use fidl_fuchsia_wlan_mlme::{self as fidl_mlme, MlmeEvent, ScanRequest};
use fidl_fuchsia_wlan_sme as fidl_sme;
use futures::channel::{mpsc, oneshot};
use log::error;
use std::collections::HashMap;
use std::sync::Arc;
use wlan_common::RadioConfig;

use super::{DeviceInfo, InfoStream, MlmeRequest, MlmeStream, Ssid};

use self::bss::{get_best_bss, get_channel_map, get_standard_map, group_networks};
use self::event::Event;
use self::rsn::get_rsna;
use self::scan::{DiscoveryScan, JoinScan, JoinScanFailure, ScanResult, ScanScheduler};
use self::state::{ConnectCommand, State};

use crate::clone_utils::clone_bss_desc;
use crate::responder::Responder;
use crate::sink::{InfoSink, MlmeSink};
use crate::timer::{self, TimedEvent};

pub use self::bss::{BssInfo, EssInfo};
pub use self::scan::DiscoveryError;

// This is necessary to trick the private-in-public checker.
// A private module is not allowed to include private types in its interface,
// even though the module itself is private and will never be exported.
// As a workaround, we add another private module with public types.
mod internal {
    use std::sync::Arc;

    use crate::client::{event::Event, ConnectionAttemptId};
    use crate::sink::{InfoSink, MlmeSink};
    use crate::timer::Timer;
    use crate::DeviceInfo;

    pub struct Context {
        pub device_info: Arc<DeviceInfo>,
        pub mlme_sink: MlmeSink,
        pub info_sink: InfoSink,
        pub(crate) timer: Timer<Event>,
        pub att_id: ConnectionAttemptId,
    }
}

use self::internal::*;

pub type TimeStream = timer::TimeStream<Event>;

pub struct ConnectConfig {
    responder: Responder<ConnectResult>,
    password: Vec<u8>,
    radio_cfg: RadioConfig,
}

// An automatically increasing sequence number that uniquely identifies a logical
// connection attempt. For example, a new connection attempt can be triggered
// by a DisassociateInd message from the MLME.
pub type ConnectionAttemptId = u64;

pub type ScanTxnId = u64;

pub struct ClientSme {
    state: Option<State>,
    scan_sched: ScanScheduler<Responder<EssDiscoveryResult>, ConnectConfig>,
    context: Context,
}

#[derive(Clone, Debug, PartialEq)]
pub enum ConnectResult {
    Success,
    Canceled,
    Failed,
    BadCredentials,
}

#[derive(Debug, PartialEq)]
pub enum ConnectFailure {
    NoMatchingBssFound,
    ScanFailure(fidl_mlme::ScanResultCodes),
    JoinFailure(fidl_mlme::JoinResultCodes),
    AuthenticationFailure(fidl_mlme::AuthenticateResultCodes),
    AssociationFailure(fidl_mlme::AssociateResultCodes),
    RsnaTimeout,
}

pub type EssDiscoveryResult = Result<Vec<EssInfo>, DiscoveryError>;

#[derive(Debug, PartialEq)]
pub enum InfoEvent {
    ConnectStarted,
    ConnectFinished {
        result: ConnectResult,
        failure: Option<ConnectFailure>,
    },
    MlmeScanStart {
        txn_id: ScanTxnId,
    },
    MlmeScanEnd {
        txn_id: ScanTxnId,
    },
    ScanDiscoveryFinished {
        bss_count: usize,
        ess_count: usize,
        num_bss_by_standard: HashMap<Standard, usize>,
        num_bss_by_channel: HashMap<u8, usize>,
    },
    AssociationStarted {
        att_id: ConnectionAttemptId,
    },
    AssociationSuccess {
        att_id: ConnectionAttemptId,
    },
    RsnaStarted {
        att_id: ConnectionAttemptId,
    },
    RsnaEstablished {
        att_id: ConnectionAttemptId,
    },
}

#[derive(Clone, Debug, PartialEq)]
pub struct Status {
    pub connected_to: Option<BssInfo>,
    pub connecting_to: Option<Ssid>,
}

#[derive(Clone, Debug, Eq, Hash, PartialEq)]
pub enum Standard {
    B,
    G,
    A,
    N,
    Ac,
}

impl ClientSme {
    pub fn new(info: DeviceInfo) -> (Self, MlmeStream, InfoStream, TimeStream) {
        let device_info = Arc::new(info);
        let (mlme_sink, mlme_stream) = mpsc::unbounded();
        let (info_sink, info_stream) = mpsc::unbounded();
        let (timer, time_stream) = timer::create_timer();
        (
            ClientSme {
                state: Some(State::Idle),
                scan_sched: ScanScheduler::new(Arc::clone(&device_info)),
                context: Context {
                    mlme_sink: MlmeSink::new(mlme_sink),
                    info_sink: InfoSink::new(info_sink),
                    device_info,
                    timer,
                    att_id: 0,
                },
            },
            mlme_stream,
            info_stream,
            time_stream,
        )
    }

    pub fn on_connect_command(
        &mut self,
        req: fidl_sme::ConnectRequest,
    ) -> oneshot::Receiver<ConnectResult> {
        let (responder, receiver) = Responder::new();
        self.context.info_sink.send(InfoEvent::ConnectStarted);
        let (canceled_token, req) = self.scan_sched.enqueue_scan_to_join(JoinScan {
            ssid: req.ssid,
            token: ConnectConfig {
                responder,
                password: req.password,
                radio_cfg: RadioConfig::from_fidl(req.radio_cfg),
            },
            scan_type: req.scan_type,
        });
        // If the new scan replaced an existing pending JoinScan, notify the existing transaction
        if let Some(token) = canceled_token {
            report_connect_finished(
                Some(token.responder),
                &self.context,
                ConnectResult::Canceled,
                None,
            );
        }
        self.send_scan_request(req);
        receiver
    }

    pub fn on_disconnect_command(&mut self) {
        self.state = self.state.take().map(|state| state.disconnect(&self.context));
    }

    pub fn on_scan_command(
        &mut self,
        scan_type: fidl_common::ScanType,
    ) -> oneshot::Receiver<EssDiscoveryResult> {
        let (responder, receiver) = Responder::new();
        let scan = DiscoveryScan::new(responder, scan_type);
        let req = self.scan_sched.enqueue_scan_to_discover(scan);
        self.send_scan_request(req);
        receiver
    }

    pub fn status(&self) -> Status {
        let status = self.state.as_ref().expect("expected state to be always present").status();
        if status.connecting_to.is_some() {
            status
        } else {
            // If the association machine is not connecting to a network, but the scanner
            // has a queued 'JoinScan', include the SSID we are trying to connect to
            Status {
                connecting_to: self.scan_sched.get_join_scan().map(|s| s.ssid.clone()),
                ..status
            }
        }
    }

    fn send_scan_request(&mut self, req: Option<ScanRequest>) {
        if let Some(req) = req {
            self.context.info_sink.send(InfoEvent::MlmeScanStart { txn_id: req.txn_id });
            self.context.mlme_sink.send(MlmeRequest::Scan(req));
        }
    }
}

impl super::Station for ClientSme {
    type Event = Event;

    fn on_mlme_event(&mut self, event: MlmeEvent) {
        self.state = self.state.take().map(|state| match event {
            MlmeEvent::OnScanResult { result } => {
                self.scan_sched.on_mlme_scan_result(result);
                state
            }
            MlmeEvent::OnScanEnd { end } => {
                self.context.info_sink.send(InfoEvent::MlmeScanEnd { txn_id: end.txn_id });
                let (result, request) = self.scan_sched.on_mlme_scan_end(end);
                self.send_scan_request(request);
                match result {
                    ScanResult::None => state,
                    ScanResult::JoinScanFinished { token, result: Ok(bss_list) } => {
                        match get_best_bss(&bss_list) {
                            Some(best_bss) => {
                                match get_rsna(
                                    &self.context.device_info,
                                    &token.password,
                                    &best_bss,
                                ) {
                                    Ok(rsna) => {
                                        let cmd = ConnectCommand {
                                            bss: Box::new(clone_bss_desc(&best_bss)),
                                            responder: Some(token.responder),
                                            rsna,
                                            radio_cfg: token.radio_cfg,
                                        };
                                        state.connect(cmd, &mut self.context)
                                    }
                                    Err(err) => {
                                        error!("cannot join BSS {:02X?} {}", best_bss.bssid, err);
                                        report_connect_finished(
                                            Some(token.responder),
                                            &self.context,
                                            ConnectResult::Failed,
                                            None,
                                        );
                                        state
                                    }
                                }
                            }
                            None => {
                                error!("no matching BSS found");
                                report_connect_finished(
                                    Some(token.responder),
                                    &self.context,
                                    ConnectResult::Failed,
                                    Some(ConnectFailure::NoMatchingBssFound),
                                );
                                state
                            }
                        }
                    }
                    ScanResult::JoinScanFinished { token, result: Err(e) } => {
                        error!("cannot join network because scan failed: {:?}", e);
                        let (result, failure) = match e {
                            JoinScanFailure::Canceled => (ConnectResult::Canceled, None),
                            JoinScanFailure::ScanFailed(code) => {
                                (ConnectResult::Failed, Some(ConnectFailure::ScanFailure(code)))
                            }
                        };
                        report_connect_finished(
                            Some(token.responder),
                            &self.context,
                            result,
                            failure,
                        );
                        state
                    }
                    ScanResult::DiscoveryFinished { tokens, result } => {
                        let result = match result {
                            Ok(bss_list) => {
                                let bss_count = bss_list.len();

                                let ess_list = group_networks(&bss_list);
                                let ess_count = ess_list.len();

                                let num_bss_by_standard = get_standard_map(&bss_list);
                                let num_bss_by_channel = get_channel_map(&bss_list);

                                self.context.info_sink.send(InfoEvent::ScanDiscoveryFinished {
                                    bss_count,
                                    ess_count,
                                    num_bss_by_standard,
                                    num_bss_by_channel,
                                });

                                Ok(ess_list)
                            }
                            Err(e) => Err(e),
                        };
                        for responder in tokens {
                            responder.respond(result.clone());
                        }
                        state
                    }
                }
            }
            other => state.on_mlme_event(other, &mut self.context),
        });
    }

    fn on_timeout(&mut self, timed_event: TimedEvent<Event>) {
        self.state = self.state.take().map(|state| match timed_event.event {
            event @ Event::EstablishingRsnaTimeout
            | event @ Event::KeyFrameExchangeTimeout { .. } => {
                state.handle_timeout(timed_event.id, event, &mut self.context)
            }
        });
    }
}

fn report_connect_finished(
    responder: Option<Responder<ConnectResult>>,
    context: &Context,
    result: ConnectResult,
    failure: Option<ConnectFailure>,
) {
    if let Some(responder) = responder {
        responder.respond(result.clone());
    }
    context.info_sink.send(InfoEvent::ConnectFinished { result, failure });
}

#[cfg(test)]
mod tests {
    use super::*;
    use fidl_fuchsia_wlan_mlme as fidl_mlme;
    use std::error::Error;
    use wlan_common::RadioConfig;

    use super::test_utils::{
        expect_info_event, fake_protected_bss_description, fake_unprotected_bss_description,
    };

    use crate::test_utils;
    use crate::Station;

    const CLIENT_ADDR: [u8; 6] = [0x7A, 0xE7, 0x76, 0xD9, 0xF2, 0x67];

    #[test]
    fn status_connecting_to() {
        let (mut sme, _mlme_stream, _info_stream, _time_stream) = create_sme();
        assert_eq!(Status { connected_to: None, connecting_to: None }, sme.status());

        // Issue a connect command and expect the status to change appropriately.
        // We also check that the association machine state is still disconnected
        // to make sure that the status comes from the scanner.
        let _recv = sme.on_connect_command(connect_req(b"foo".to_vec(), vec![]));
        assert_eq!(None, sme.state.as_ref().unwrap().status().connecting_to);
        assert_eq!(
            Status { connected_to: None, connecting_to: Some(b"foo".to_vec()) },
            sme.status()
        );

        // Push a fake scan result into SME. We should still be connecting to "foo",
        // but the status should now come from the state machine and not from the scanner.
        sme.on_mlme_event(MlmeEvent::OnScanResult {
            result: fidl_mlme::ScanResult {
                txn_id: 1,
                bss: fake_unprotected_bss_description(b"foo".to_vec()),
            },
        });
        sme.on_mlme_event(MlmeEvent::OnScanEnd {
            end: fidl_mlme::ScanEnd { txn_id: 1, code: fidl_mlme::ScanResultCodes::Success },
        });
        assert_eq!(Some(b"foo".to_vec()), sme.state.as_ref().unwrap().status().connecting_to);
        assert_eq!(
            Status { connected_to: None, connecting_to: Some(b"foo".to_vec()) },
            sme.status()
        );

        // Even if we scheduled a scan to connect to another network "bar", we should
        // still report that we are connecting to "foo".
        let _recv2 = sme.on_connect_command(connect_req(b"bar".to_vec(), vec![]));
        assert_eq!(
            Status { connected_to: None, connecting_to: Some(b"foo".to_vec()) },
            sme.status()
        );

        // Simulate that joining "foo" failed. We should now be connecting to "bar".
        sme.on_mlme_event(MlmeEvent::JoinConf {
            resp: fidl_mlme::JoinConfirm {
                result_code: fidl_mlme::JoinResultCodes::JoinFailureTimeout,
            },
        });
        assert_eq!(
            Status { connected_to: None, connecting_to: Some(b"bar".to_vec()) },
            sme.status()
        );
    }

    #[test]
    fn connecting_password_supplied_for_protected_network() {
        let (mut sme, mut mlme_stream, _info_stream, _time_stream) = create_sme();
        assert_eq!(Status { connected_to: None, connecting_to: None }, sme.status());

        // Issue a connect command and verify that connecting_to status is changed for upper
        // layer (but not underlying state machine) and a scan request is sent to MLME.
        let req = connect_req(b"foo".to_vec(), b"somepass".to_vec());
        let _recv = sme.on_connect_command(req);
        assert_eq!(None, sme.state.as_ref().unwrap().status().connecting_to);
        assert_eq!(
            Status { connected_to: None, connecting_to: Some(b"foo".to_vec()) },
            sme.status()
        );

        if let Ok(Some(MlmeRequest::Scan(..))) = mlme_stream.try_next() {
            // expected path; nothing to do
        } else {
            panic!("expect scan request to MLME");
        }

        // Simulate scan end and verify that underlying state machine's status is changed,
        // and a join request is sent to MLME.
        sme.on_mlme_event(MlmeEvent::OnScanResult {
            result: fidl_mlme::ScanResult {
                txn_id: 1,
                bss: fake_protected_bss_description(b"foo".to_vec()),
            },
        });
        sme.on_mlme_event(MlmeEvent::OnScanEnd {
            end: fidl_mlme::ScanEnd { txn_id: 1, code: fidl_mlme::ScanResultCodes::Success },
        });
        assert_eq!(Some(b"foo".to_vec()), sme.state.as_ref().unwrap().status().connecting_to);
        assert_eq!(
            Status { connected_to: None, connecting_to: Some(b"foo".to_vec()) },
            sme.status()
        );

        if let Ok(Some(MlmeRequest::Join(..))) = mlme_stream.try_next() {
            // expected path; nothing to do
        } else {
            panic!("expect join request to MLME");
        }
    }

    #[test]
    fn connecting_password_supplied_for_unprotected_network() {
        let (mut sme, mut mlme_stream, _info_stream, _time_stream) = create_sme();
        assert_eq!(Status { connected_to: None, connecting_to: None }, sme.status());

        let req = connect_req(b"foo".to_vec(), b"somepass".to_vec());
        let mut connect_fut = sme.on_connect_command(req);
        assert_eq!(None, sme.state.as_ref().unwrap().status().connecting_to);
        assert_eq!(
            Status { connected_to: None, connecting_to: Some(b"foo".to_vec()) },
            sme.status()
        );

        // Push a fake scan result into SME. We should not attempt to connect
        // because a password was supplied for unprotected network. So both the
        // SME client and underlying state machine should report not connecting
        // anymore.
        sme.on_mlme_event(MlmeEvent::OnScanResult {
            result: fidl_mlme::ScanResult {
                txn_id: 1,
                bss: fake_unprotected_bss_description(b"foo".to_vec()),
            },
        });
        sme.on_mlme_event(MlmeEvent::OnScanEnd {
            end: fidl_mlme::ScanEnd { txn_id: 1, code: fidl_mlme::ScanResultCodes::Success },
        });
        assert_eq!(None, sme.state.as_ref().unwrap().status().connecting_to);
        assert_eq!(Status { connected_to: None, connecting_to: None }, sme.status());

        // No join request should be sent to MLME
        loop {
            match mlme_stream.try_next() {
                Ok(event) => match event {
                    Some(MlmeRequest::Join(..)) => panic!("unexpected join request to MLME"),
                    None => break,
                    _ => (),
                },
                Err(e) => {
                    assert_eq!(e.description(), "receiver channel is empty");
                    break;
                }
            }
        }

        // User should get a message that connection failed
        assert_eq!(Ok(Some(ConnectResult::Failed)), connect_fut.try_recv());
    }

    #[test]
    fn connecting_no_password_supplied_for_protected_network() {
        let (mut sme, mut mlme_stream, _info_stream, _time_stream) = create_sme();
        assert_eq!(Status { connected_to: None, connecting_to: None }, sme.status());

        let req = connect_req(b"foo".to_vec(), vec![]);
        let mut connect_fut = sme.on_connect_command(req);
        assert_eq!(None, sme.state.as_ref().unwrap().status().connecting_to);
        assert_eq!(
            Status { connected_to: None, connecting_to: Some(b"foo".to_vec()) },
            sme.status()
        );

        // Push a fake scan result into SME. We should not attempt to connect
        // because no password was supplied for a protected network.
        sme.on_mlme_event(MlmeEvent::OnScanResult {
            result: fidl_mlme::ScanResult {
                txn_id: 1,
                bss: fake_protected_bss_description(b"foo".to_vec()),
            },
        });
        sme.on_mlme_event(MlmeEvent::OnScanEnd {
            end: fidl_mlme::ScanEnd { txn_id: 1, code: fidl_mlme::ScanResultCodes::Success },
        });
        assert_eq!(None, sme.state.as_ref().unwrap().status().connecting_to);
        assert_eq!(Status { connected_to: None, connecting_to: None }, sme.status());

        // No join request should be sent to MLME
        loop {
            match mlme_stream.try_next() {
                Ok(event) => match event {
                    Some(MlmeRequest::Join(..)) => panic!("unexpected join request sent to MLME"),
                    None => break,
                    _ => (),
                },
                Err(e) => {
                    assert_eq!(e.description(), "receiver channel is empty");
                    break;
                }
            }
        }

        // User should get a message that connection failed
        assert_eq!(Ok(Some(ConnectResult::Failed)), connect_fut.try_recv());
    }

    #[test]
    fn connecting_generates_info_events() {
        let (mut sme, _mlme_stream, mut info_stream, _time_stream) = create_sme();

        let _recv = sme.on_connect_command(connect_req(b"foo".to_vec(), vec![]));
        expect_info_event(&mut info_stream, InfoEvent::ConnectStarted);
        expect_info_event(&mut info_stream, InfoEvent::MlmeScanStart { txn_id: 1 });

        sme.on_mlme_event(MlmeEvent::OnScanResult {
            result: fidl_mlme::ScanResult {
                txn_id: 1,
                bss: fake_unprotected_bss_description(b"foo".to_vec()),
            },
        });
        sme.on_mlme_event(MlmeEvent::OnScanEnd {
            end: fidl_mlme::ScanEnd { txn_id: 1, code: fidl_mlme::ScanResultCodes::Success },
        });
        expect_info_event(&mut info_stream, InfoEvent::MlmeScanEnd { txn_id: 1 });
        expect_info_event(&mut info_stream, InfoEvent::AssociationStarted { att_id: 1 });
    }

    fn connect_req(ssid: Ssid, password: Vec<u8>) -> fidl_sme::ConnectRequest {
        fidl_sme::ConnectRequest {
            ssid,
            password,
            radio_cfg: RadioConfig::default().to_fidl(),
            scan_type: fidl_common::ScanType::Passive,
        }
    }

    fn create_sme() -> (ClientSme, MlmeStream, InfoStream, TimeStream) {
        ClientSme::new(test_utils::fake_device_info(CLIENT_ADDR))
    }
}
