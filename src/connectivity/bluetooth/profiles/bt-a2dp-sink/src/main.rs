// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![recursion_limit = "512"]

use {
    anyhow::{format_err, Context as _, Error},
    argh::FromArgs,
    async_helpers::component_lifecycle::ComponentLifecycleServer,
    bt_a2dp::{codec::MediaCodecConfig, media_types::*, peer::ControllerPool, stream},
    bt_a2dp_metrics as metrics,
    bt_avdtp::{self as avdtp, ServiceCapability, ServiceCategory, StreamEndpoint},
    fidl::encoding::Decodable,
    fidl::endpoints::create_request_stream,
    fidl_fuchsia_bluetooth_bredr as bredr,
    fidl_fuchsia_bluetooth_component::LifecycleState,
    fidl_fuchsia_media_sessions2 as sessions2,
    fuchsia_async::{self as fasync, DurationExt},
    fuchsia_bluetooth::{
        profile::find_profile_descriptors,
        types::{Channel, PeerId, Uuid},
    },
    fuchsia_cobalt::{CobaltConnector, CobaltSender, ConnectionType},
    fuchsia_component::server::ServiceFs,
    fuchsia_inspect as inspect,
    fuchsia_inspect_derive::Inspect,
    fuchsia_zircon as zx,
    futures::{select, StreamExt, TryStreamExt},
    log::{info, trace, warn},
    parking_lot::Mutex,
    std::{convert::TryFrom, convert::TryInto, sync::Arc},
};

use crate::avrcp_relay::AvrcpRelay;
use crate::connected_peers::ConnectedPeers;

mod avrcp_relay;
mod connected_peers;
mod latm;
mod player;
mod sink_task;
mod volume_relay;

/// Make the SDP definition for the A2DP sink service.
fn make_profile_service_definition() -> bredr::ServiceDefinition {
    bredr::ServiceDefinition {
        service_class_uuids: Some(vec![Uuid::new16(0x110B).into()]), // Audio Sink UUID
        protocol_descriptor_list: Some(vec![
            bredr::ProtocolDescriptor {
                protocol: bredr::ProtocolIdentifier::L2Cap,
                params: vec![bredr::DataElement::Uint16(bredr::PSM_AVDTP)],
            },
            bredr::ProtocolDescriptor {
                protocol: bredr::ProtocolIdentifier::Avdtp,
                params: vec![bredr::DataElement::Uint16(0x0103)], // Indicate v1.3
            },
        ]),
        profile_descriptors: Some(vec![bredr::ProfileDescriptor {
            profile_id: bredr::ServiceClassProfileIdentifier::AdvancedAudioDistribution,
            major_version: 1,
            minor_version: 2,
        }]),
        ..Decodable::new_empty()
    }
}

// SDP Attribute ID for the Supported Features of A2DP.
// Defined in Assigned Numbers for SDP
// https://www.bluetooth.com/specifications/assigned-numbers/service-discovery
const ATTR_A2DP_SUPPORTED_FEATURES: u16 = 0x0311;

// Arbitrarily chosen ID for the SBC stream endpoint.
const SBC_SEID: u8 = 6;

// Arbitrarily chosen ID for the AAC stream endpoint.
const AAC_SEID: u8 = 7;

pub const DEFAULT_SAMPLE_RATE: u32 = 48000;
pub const DEFAULT_SESSION_ID: u64 = 0;

// Duration for A2DP-SNK to wait before assuming role of the initiator.
// If an L2CAP signaling channel has not been established by this time, A2DP-Sink will
// create the signaling channel, configure, open and start the stream.
const INITIATOR_DELAY: zx::Duration = zx::Duration::from_seconds(2);

fn find_codec_cap<'a>(endpoint: &'a StreamEndpoint) -> Option<&'a ServiceCapability> {
    endpoint.capabilities().iter().find(|cap| cap.category() == ServiceCategory::MediaCodec)
}

struct StreamsBuilder {
    cobalt_sender: CobaltSender,
    domain: Option<String>,
    aac_available: bool,
}

impl StreamsBuilder {
    async fn system_playable(
        cobalt_sender: CobaltSender,
        domain: Option<String>,
    ) -> Result<Self, Error> {
        // TODO(BT-533): detect codecs, add streams for each codec
        // SBC is required
        let sbc_endpoint = Self::build_sbc_endpoint(avdtp::EndpointType::Sink)?;
        let codec_cap = find_codec_cap(&sbc_endpoint).expect("just built");
        let sbc_config = MediaCodecConfig::try_from(codec_cap)?;
        if let Err(e) = player::Player::test_playable(&sbc_config).await {
            warn!("Can't play required SBC audio: {}", e);
            return Err(e);
        }

        let aac_endpoint = Self::build_aac_endpoint(avdtp::EndpointType::Sink)?;
        let codec_cap = find_codec_cap(&aac_endpoint).expect("just built");
        let aac_config = MediaCodecConfig::try_from(codec_cap)?;
        let aac_available = player::Player::test_playable(&aac_config).await.is_ok();

        Ok(Self { cobalt_sender, domain, aac_available })
    }

    fn build_sbc_endpoint(
        endpoint_type: avdtp::EndpointType,
    ) -> avdtp::Result<avdtp::StreamEndpoint> {
        let sbc_codec_info = SbcCodecInfo::new(
            SbcSamplingFrequency::MANDATORY_SNK,
            SbcChannelMode::MANDATORY_SNK,
            SbcBlockCount::MANDATORY_SNK,
            SbcSubBands::MANDATORY_SNK,
            SbcAllocation::MANDATORY_SNK,
            SbcCodecInfo::BITPOOL_MIN,
            SbcCodecInfo::BITPOOL_MAX,
        )?;
        trace!("Supported SBC codec parameters: {:?}.", sbc_codec_info);

        avdtp::StreamEndpoint::new(
            SBC_SEID,
            avdtp::MediaType::Audio,
            endpoint_type,
            vec![
                ServiceCapability::MediaTransport,
                ServiceCapability::MediaCodec {
                    media_type: avdtp::MediaType::Audio,
                    codec_type: avdtp::MediaCodecType::AUDIO_SBC,
                    codec_extra: sbc_codec_info.to_bytes().to_vec(),
                },
            ],
        )
    }

    fn build_aac_endpoint(
        endpoint_type: avdtp::EndpointType,
    ) -> avdtp::Result<avdtp::StreamEndpoint> {
        let aac_codec_info = AacCodecInfo::new(
            AacObjectType::MANDATORY_SNK,
            AacSamplingFrequency::MANDATORY_SNK,
            AacChannels::MANDATORY_SNK,
            true,
            0, // 0 = Unknown constant bitrate support (A2DP Sec. 4.5.2.4)
        )?;

        trace!("Supported codec parameters: {:?}.", aac_codec_info);

        avdtp::StreamEndpoint::new(
            AAC_SEID,
            avdtp::MediaType::Audio,
            endpoint_type,
            vec![
                ServiceCapability::MediaTransport,
                ServiceCapability::MediaCodec {
                    media_type: avdtp::MediaType::Audio,
                    codec_type: avdtp::MediaCodecType::AUDIO_AAC,
                    codec_extra: aac_codec_info.to_bytes().to_vec(),
                },
            ],
        )
    }

    async fn setup_audio_session(
        domain: Option<String>,
    ) -> Result<(sessions2::PlayerRequestStream, u64), Error> {
        let publisher =
            fuchsia_component::client::connect_to_service::<sessions2::PublisherMarker>()
                .context("Failed to connect to MediaSession interface")?;

        let (player_client, player_request_stream) = create_request_stream()?;

        let registration = sessions2::PlayerRegistration {
            domain: domain.or(Some("Bluetooth".to_string())),
            ..Decodable::new_empty()
        };

        let session_id = publisher.publish(player_client, registration).await?;

        Ok((player_request_stream, session_id))
    }

    fn into_session_gen(self) -> Box<connected_peers::PeerSessionFn> {
        Box::new(move |peer_id: &PeerId| {
            let domain = self.domain.clone();
            let peer_id = peer_id.clone();
            let cobalt_sender = self.cobalt_sender.clone();
            let aac_available = self.aac_available.clone();
            let gen_fut = async move {
                let (player_request_stream, session_id) = Self::setup_audio_session(domain).await?;
                info!("Session ID: {}", session_id);
                let avrcp_task = AvrcpRelay::start(peer_id.clone(), player_request_stream)
                    .unwrap_or(fasync::Task::spawn(async {}));

                let sink_task_builder = sink_task::SinkTaskBuilder::new(cobalt_sender, session_id);
                let mut streams = stream::Streams::new();
                let sbc_endpoint = Self::build_sbc_endpoint(avdtp::EndpointType::Sink)?;
                streams.insert(stream::Stream::build(sbc_endpoint, sink_task_builder.clone()));

                if aac_available {
                    let aac_endpoint = Self::build_aac_endpoint(avdtp::EndpointType::Sink)?;
                    streams.insert(stream::Stream::build(aac_endpoint, sink_task_builder.clone()));
                }
                Ok((streams, avrcp_task))
            };
            fasync::Task::spawn(gen_fut)
        })
    }
}

/// Establishes the signaling channel after the delay specified by `timer_expired`.
async fn connect_after_timeout(
    peer_id: PeerId,
    peers: Arc<Mutex<ConnectedPeers>>,
    controller_pool: Arc<Mutex<ControllerPool>>,
    profile_svc: bredr::ProfileProxy,
    channel_mode: bredr::ChannelMode,
) {
    trace!(
        "A2DP sink - waiting {}s before connecting to peer {}.",
        INITIATOR_DELAY.into_seconds(),
        peer_id
    );
    fuchsia_async::Timer::new(INITIATOR_DELAY.after_now()).await;
    if peers.lock().is_connected(&peer_id) {
        return;
    }

    trace!("Peer has not established connection. A2DP sink assuming the INT role.");
    let channel = match profile_svc
        .connect(
            &mut peer_id.into(),
            &mut bredr::ConnectParameters::L2cap(bredr::L2capParameters {
                psm: Some(bredr::PSM_AVDTP),
                parameters: Some(bredr::ChannelParameters {
                    channel_mode: Some(channel_mode),
                    ..Decodable::new_empty()
                }),
                ..Decodable::new_empty()
            }),
        )
        .await
    {
        Err(e) => {
            warn!("FIDL error creating channel: {:?}", e);
            return;
        }
        Ok(Err(e)) => {
            warn!("Couldn't connect to {}: {:?}", peer_id, e);
            return;
        }
        Ok(Ok(channel)) => channel,
    };

    let channel = match channel.try_into() {
        Err(e) => {
            warn!("Didn't get channel from peer {}: {}", peer_id, e);
            return;
        }
        Ok(chan) => chan,
    };

    handle_connection(
        &peer_id,
        channel,
        /* initiate = */ true,
        &mut peers.lock(),
        &mut controller_pool.lock(),
    )
    .await;
}

/// Handles incoming peer connections
async fn handle_connection(
    peer_id: &PeerId,
    channel: Channel,
    initiate: bool,
    peers: &mut ConnectedPeers,
    controller_pool: &mut ControllerPool,
) {
    info!("Connection from {}: {:?}!", peer_id, channel);
    peers.connected(peer_id.clone(), channel, initiate).await;
    if let Some(peer) = peers.get_weak(&peer_id) {
        // Add the controller to the peers
        controller_pool.peer_connected(peer_id.clone(), peer);
    }
}

/// Handles found services. Stores the found information and then spawns a task which will
/// assume initator role after a delay.
fn handle_services_found(
    peer_id: &PeerId,
    attributes: &[bredr::Attribute],
    peers: Arc<Mutex<ConnectedPeers>>,
    controller_pool: Arc<Mutex<ControllerPool>>,
    profile_svc: bredr::ProfileProxy,
    channel_mode: bredr::ChannelMode,
) {
    info!("Audio Source found on {}, attributes: {:?}", peer_id, attributes);

    let profile = match find_profile_descriptors(attributes) {
        Ok(profiles) => profiles.into_iter().next().expect("at least one profile descriptor"),
        Err(_) => {
            info!("Couldn't find profile in peer {} search results, ignoring.", peer_id);
            return;
        }
    };

    peers.lock().found(peer_id.clone(), profile);

    if peers.lock().is_connected(&peer_id) {
        return;
    }

    fasync::Task::local(connect_after_timeout(
        peer_id.clone(),
        peers.clone(),
        controller_pool.clone(),
        profile_svc,
        channel_mode,
    ))
    .detach();
}

/// Parses the ChannelMode from the String argument.
///
/// Returns an Error if the provided argument is an invalid string.
fn channel_mode_from_arg(channel_mode: Option<String>) -> Result<bredr::ChannelMode, Error> {
    match channel_mode {
        None => Ok(bredr::ChannelMode::Basic),
        Some(s) if s == "basic" => Ok(bredr::ChannelMode::Basic),
        Some(s) if s == "ertm" => Ok(bredr::ChannelMode::EnhancedRetransmission),
        Some(s) => return Err(format_err!("invalid channel mode: {}", s)),
    }
}

/// Options available from the command line
#[derive(FromArgs)]
#[argh(description = "Bluetooth Advanced Audio Distribution Profile: Sink")]
struct Opt {
    #[argh(option)]
    /// published Media Session Domain (optional, defaults to a native Fuchsia session)
    // TODO - Point to any media documentation about domains
    domain: Option<String>,

    #[argh(option, short = 'c', long = "channelmode")]
    /// channel mode preferred for the AVDTP signaling channel (optional, defaults to "basic", values: "basic", "ertm").
    channel_mode: Option<String>,
}

#[fasync::run_singlethreaded]
async fn main() -> Result<(), Error> {
    let opts: Opt = argh::from_env();

    fuchsia_syslog::init_with_tags(&["a2dp-sink"]).expect("Can't init logger");
    fuchsia_trace_provider::trace_provider_create_with_fdio();

    let signaling_channel_mode = channel_mode_from_arg(opts.channel_mode)?;
    let controller_pool = Arc::new(Mutex::new(ControllerPool::new()));

    let mut fs = ServiceFs::new();

    let inspect = inspect::Inspector::new();
    inspect.serve(&mut fs)?;

    let mut lifecycle = ComponentLifecycleServer::spawn();
    fs.dir("svc").add_fidl_service(lifecycle.fidl_service());

    let pool_clone = controller_pool.clone();
    fs.dir("svc").add_fidl_service(move |s| pool_clone.lock().connected(s));

    if let Err(e) = fs.take_and_serve_directory_handle() {
        warn!("Unable to serve service directory: {}", e);
    }
    let _servicefs_task = fasync::Task::spawn(fs.collect::<()>());

    let abs_vol_relay = volume_relay::VolumeRelay::start();
    if let Err(e) = &abs_vol_relay {
        info!("Failed to start AbsoluteVolume Relay: {:?}", e);
    }

    let cobalt_logger: CobaltSender = {
        let (sender, reporter) =
            CobaltConnector::default().serve(ConnectionType::project_id(metrics::PROJECT_ID));
        fasync::Task::spawn(reporter).detach();
        sender
    };

    let stream_gen = StreamsBuilder::system_playable(cobalt_logger.clone(), opts.domain).await?;

    let profile_svc = fuchsia_component::client::connect_to_service::<bredr::ProfileMarker>()
        .context("Failed to connect to Bluetooth Profile service")?;

    let mut peers = connected_peers::ConnectedPeers::new(
        stream_gen.into_session_gen(),
        profile_svc.clone(),
        cobalt_logger.clone(),
    );
    if let Err(e) = peers.iattach(&inspect.root(), "connected") {
        info!("Failed to attach to inspect: {:?}", e);
    }

    let peers = Arc::new(Mutex::new(peers));

    let service_defs = vec![make_profile_service_definition()];

    let (connect_client, connect_requests) =
        create_request_stream().context("ConnectionReceiver creation")?;

    let _ = profile_svc.advertise(
        &mut service_defs.into_iter(),
        bredr::ChannelParameters {
            channel_mode: Some(signaling_channel_mode),
            ..Decodable::new_empty()
        },
        connect_client,
    );

    const ATTRS: [u16; 4] = [
        bredr::ATTR_PROTOCOL_DESCRIPTOR_LIST,
        bredr::ATTR_SERVICE_CLASS_ID_LIST,
        bredr::ATTR_BLUETOOTH_PROFILE_DESCRIPTOR_LIST,
        ATTR_A2DP_SUPPORTED_FEATURES,
    ];

    let (results_client, results_requests) =
        create_request_stream().context("SearchResults creation")?;

    profile_svc.search(
        bredr::ServiceClassProfileIdentifier::AudioSource,
        &ATTRS,
        results_client,
    )?;

    lifecycle.set(LifecycleState::Ready).await.expect("lifecycle server to set value");

    handle_profile_events(
        peers,
        controller_pool,
        profile_svc,
        signaling_channel_mode,
        connect_requests,
        results_requests,
    )
    .await
}

async fn handle_profile_events(
    peers: Arc<Mutex<ConnectedPeers>>,
    controller_pool: Arc<Mutex<ControllerPool>>,
    profile_svc: bredr::ProfileProxy,
    channel_mode: bredr::ChannelMode,
    mut connect_requests: bredr::ConnectionReceiverRequestStream,
    mut results_requests: bredr::SearchResultsRequestStream,
) -> Result<(), Error> {
    loop {
        select! {
            connect_request = connect_requests.try_next() => {
                let connected = match connect_request? {
                    None => return Err(format_err!("BR/EDR ended service registration")),
                    Some(request) => request,
                };
                let bredr::ConnectionReceiverRequest::Connected { peer_id, channel, .. } = connected;
                handle_connection(
                    &peer_id.into(),
                    channel.try_into()?,
                    /* initiate = */ false,
                    &mut peers.lock(),
                    &mut controller_pool.lock()).await;
            }
            results_request = results_requests.try_next() => {
                let result = match results_request? {
                    None => return Err(format_err!("BR/EDR ended service search")),
                    Some(request) => request,
                };
                let bredr::SearchResultsRequest::ServiceFound { peer_id, protocol, attributes, responder } = result;

                handle_services_found(&peer_id.into(), &attributes, peers.clone(), controller_pool.clone(), profile_svc.clone(), channel_mode.clone());
                responder.send()?;
            }
            complete => break,
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    use fidl::endpoints::create_proxy_and_stream;
    use fidl_fuchsia_bluetooth_bredr::{
        ConnectionReceiverMarker, ProfileRequest, ProfileRequestStream, SearchResultsMarker,
    };
    use fidl_fuchsia_cobalt::CobaltEvent;
    use fuchsia_bluetooth::types::PeerId;
    use futures::channel::mpsc;
    use futures::{pin_mut, task::Poll, StreamExt};
    use matches::assert_matches;

    pub(crate) fn fake_cobalt_sender() -> (CobaltSender, mpsc::Receiver<CobaltEvent>) {
        const BUFFER_SIZE: usize = 100;
        let (sender, receiver) = mpsc::channel(BUFFER_SIZE);
        (CobaltSender::new(sender), receiver)
    }

    fn run_to_stalled(exec: &mut fasync::Executor) {
        let _ = exec.run_until_stalled(&mut futures::future::pending::<()>());
    }

    fn no_streams_gen() -> Box<connected_peers::PeerSessionFn> {
        Box::new(|_peer_id| {
            fasync::Task::spawn(async {
                Ok((stream::Streams::new(), fasync::Task::spawn(async {})))
            })
        })
    }

    fn setup_connected_peer_test() -> (
        fasync::Executor,
        Arc<Mutex<ConnectedPeers>>,
        bredr::ProfileProxy,
        ProfileRequestStream,
        Arc<Mutex<ControllerPool>>,
    ) {
        let exec = fasync::Executor::new_with_fake_time().expect("executor should build");
        let (proxy, stream) = create_proxy_and_stream::<bredr::ProfileMarker>()
            .expect("Profile proxy should be created");
        let (cobalt_sender, _) = fake_cobalt_sender();
        let peers = Arc::new(Mutex::new(ConnectedPeers::new(
            no_streams_gen(),
            proxy.clone(),
            cobalt_sender,
        )));

        let controller_pool = Arc::new(Mutex::new(ControllerPool::new()));

        (exec, peers, proxy, stream, controller_pool)
    }

    #[test]
    fn test_responds_to_search_results() {
        let (mut exec, peers, profile_proxy, _profile_stream, controller_pool) =
            setup_connected_peer_test();
        let (results_proxy, results_stream) = create_proxy_and_stream::<SearchResultsMarker>()
            .expect("SearchResults proxy should be created");
        let (_connect_proxy, connect_stream) =
            create_proxy_and_stream::<ConnectionReceiverMarker>()
                .expect("ConnectionReceiver proxy should be created");

        let handler_fut = handle_profile_events(
            peers,
            controller_pool,
            profile_proxy,
            bredr::ChannelMode::Basic,
            connect_stream,
            results_stream,
        );
        pin_mut!(handler_fut);

        let res = exec.run_until_stalled(&mut handler_fut);
        assert!(res.is_pending());

        // Report a search result, which should be replied to.
        let mut attributes = vec![];
        let results_fut =
            results_proxy.service_found(&mut PeerId(1).into(), None, &mut attributes.iter_mut());
        pin_mut!(results_fut);

        let res = exec.run_until_stalled(&mut handler_fut);
        assert!(res.is_pending());
        match exec.run_until_stalled(&mut results_fut) {
            Poll::Ready(Ok(())) => {}
            x => panic!("Expected a response from the result, got {:?}", x),
        };
    }

    #[test]
    /// build_local_streams should fail because it can't start the SBC encoder, because
    /// MediaPlayer isn't available in the test environment.
    fn test_sbc_unavailable_error() {
        let mut exec = fasync::Executor::new().expect("executor should build");

        let (sender, _) = fake_cobalt_sender();
        let mut streams_fut = Box::pin(StreamsBuilder::system_playable(sender, None));

        let streams = exec.run_singlethreaded(&mut streams_fut);

        assert!(streams.is_err(), "Stream building should fail when it can't reach MediaPlayer");
    }

    #[test]
    /// Tests that A2DP sink assumes the initiator role when a peer is found, but
    /// not connected, and the timeout completes.
    fn wait_to_initiate_success_with_no_connected_peer() {
        let (mut exec, peers, proxy, mut prof_stream, controller_pool) =
            setup_connected_peer_test();
        // Initialize context to a fixed point in time.
        exec.set_fake_time(fasync::Time::from_nanos(1000000000));
        let peer_id = PeerId(1);

        // Simulate getting the service found event.
        let attributes = vec![bredr::Attribute {
            id: bredr::ATTR_BLUETOOTH_PROFILE_DESCRIPTOR_LIST,
            element: bredr::DataElement::Sequence(vec![Some(Box::new(
                bredr::DataElement::Sequence(vec![
                    Some(Box::new(
                        Uuid::from(bredr::ServiceClassProfileIdentifier::AudioSource).into(),
                    )),
                    Some(Box::new(bredr::DataElement::Uint16(0x0103))), // Version 1.3
                ]),
            ))]),
        }];
        handle_services_found(
            &peer_id,
            &attributes,
            peers.clone(),
            controller_pool.clone(),
            proxy.clone(),
            bredr::ChannelMode::Basic,
        );

        run_to_stalled(&mut exec);

        // At this point, a remote peer was found, but hasn't connected yet. There
        // should be no entry for it.
        assert!(!peers.lock().is_connected(&peer_id));

        // Fast forward time by 5 seconds. In this time, the remote peer has not
        // connected.
        exec.set_fake_time(fasync::Time::from_nanos(6000000000));
        exec.wake_expired_timers();
        run_to_stalled(&mut exec);

        // After fast forwarding time, expect and handle the `connect` request
        // because A2DP-sink should be initiating.
        let (_test, transport) = Channel::create();
        let request = exec.run_until_stalled(&mut prof_stream.next());
        match request {
            Poll::Ready(Some(Ok(ProfileRequest::Connect { peer_id, responder, .. }))) => {
                assert_eq!(PeerId(1), peer_id.into());
                let channel = transport.try_into().unwrap();
                responder.send(&mut Ok(channel)).expect("responder sends");
            }
            x => panic!("Should have sent a connect request, but got {:?}", x),
        };
        run_to_stalled(&mut exec);

        // The remote peer did not connect to us, A2DP Sink should initiate a connection
        // and insert into `peers`.
        assert!(peers.lock().is_connected(&peer_id));
    }

    #[test]
    /// Tests that A2DP sink does not assume the initiator role when a peer connects
    /// before `INITIATOR_DELAY` timeout completes.
    fn wait_to_initiate_returns_early_with_connected_peer() {
        let (mut exec, peers, proxy, mut prof_stream, controller_pool) =
            setup_connected_peer_test();
        // Initialize context to a fixed point in time.
        exec.set_fake_time(fasync::Time::from_nanos(1000000000));
        let peer_id = PeerId(1);

        // Simulate getting the service found event.
        let attributes = vec![bredr::Attribute {
            id: bredr::ATTR_BLUETOOTH_PROFILE_DESCRIPTOR_LIST,
            element: bredr::DataElement::Sequence(vec![Some(Box::new(
                bredr::DataElement::Sequence(vec![
                    Some(Box::new(
                        Uuid::from(bredr::ServiceClassProfileIdentifier::AudioSource).into(),
                    )),
                    Some(Box::new(bredr::DataElement::Uint16(0x0103))), // Version 1.3
                ]),
            ))]),
        }];
        handle_services_found(
            &peer_id,
            &attributes,
            peers.clone(),
            controller_pool.clone(),
            proxy.clone(),
            bredr::ChannelMode::Basic,
        );

        // At this point, a remote peer was found, but hasn't connected yet. There
        // should be no entry for it.
        assert!(!peers.lock().is_connected(&peer_id));

        // Fast forward time by .5 seconds. The threshold is 1 second, so the timer
        // to initiate connections has not been triggered.
        exec.set_fake_time(fasync::Time::from_nanos(1500000000));
        exec.wake_expired_timers();
        run_to_stalled(&mut exec);

        // A peer connects before the timeout.
        let (_remote, transport) = Channel::create();
        {
            let mut peers_lock = peers.lock();
            let mut controller_pool_lock = controller_pool.lock();
            let connection_fut = handle_connection(
                &peer_id,
                transport,
                /* initiate = */ false,
                &mut peers_lock,
                &mut controller_pool_lock,
            );
            pin_mut!(connection_fut);

            assert!(exec.run_until_stalled(&mut connection_fut).is_ready());
        }

        run_to_stalled(&mut exec);

        // The remote peer connected to us, and should be in the map.
        assert!(peers.lock().is_connected(&peer_id));

        // Fast forward time by 4.5 seconds. Ensure no outbound connection is initiated
        // by us, since the remote peer has assumed the INT role.
        exec.set_fake_time(fasync::Time::from_nanos(6000000000));
        exec.wake_expired_timers();
        run_to_stalled(&mut exec);

        let request = exec.run_until_stalled(&mut prof_stream.next());
        match request {
            Poll::Ready(x) => panic!("There should be no l2cap connection requests: {:?}", x),
            Poll::Pending => {}
        };
        run_to_stalled(&mut exec);
    }

    #[test]
    fn test_channel_mode_from_arg() {
        let channel_string = None;
        assert_matches!(channel_mode_from_arg(channel_string), Ok(bredr::ChannelMode::Basic));

        let channel_string = Some("basic".to_string());
        assert_matches!(channel_mode_from_arg(channel_string), Ok(bredr::ChannelMode::Basic));

        let channel_string = Some("ertm".to_string());
        assert_matches!(
            channel_mode_from_arg(channel_string),
            Ok(bredr::ChannelMode::EnhancedRetransmission)
        );

        let channel_string = Some("foobar123".to_string());
        assert!(channel_mode_from_arg(channel_string).is_err());
    }
}
