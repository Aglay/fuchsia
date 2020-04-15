// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![recursion_limit = "512"]

use {
    anyhow::{format_err, Context as _, Error},
    argh::FromArgs,
    async_helpers::component_lifecycle::ComponentLifecycleServer,
    bt_a2dp::media_types::*,
    bt_avdtp::{self as avdtp, AvdtpControllerPool},
    fidl::{encoding::Decodable, endpoints::create_request_stream},
    fidl_fuchsia_bluetooth_bredr::*,
    fidl_fuchsia_bluetooth_component::LifecycleState,
    fuchsia_async as fasync,
    fuchsia_bluetooth::{
        detachable_map::{DetachableMap, DetachableWeak},
        profile::find_profile_descriptors,
        types::{PeerId, Uuid},
    },
    fuchsia_component::server::ServiceFs,
    fuchsia_syslog::{self, fx_log_info, fx_log_warn, fx_vlog},
    fuchsia_zircon as zx,
    futures::{self, select, StreamExt, TryStreamExt},
    parking_lot::Mutex,
    std::{
        convert::{TryFrom, TryInto},
        sync::Arc,
    },
};

mod encoding;
mod media_task;
mod pcm_audio;
mod peer;
mod sources;
mod stream;

use crate::pcm_audio::PcmAudio;
use crate::peer::Peer;
use sources::AudioSourceType;

/// Make the SDP definition for the A2DP source service.
fn make_profile_service_definition() -> ServiceDefinition {
    ServiceDefinition {
        service_class_uuids: Some(vec![Uuid::new16(0x110A).into()]), // Audio Source UUID
        protocol_descriptor_list: Some(vec![
            ProtocolDescriptor {
                protocol: ProtocolIdentifier::L2Cap,
                params: vec![DataElement::Uint16(PSM_AVDTP)],
            },
            ProtocolDescriptor {
                protocol: ProtocolIdentifier::Avdtp,
                params: vec![DataElement::Uint16(0x0103)], // Indicate v1.3
            },
        ]),
        profile_descriptors: Some(vec![ProfileDescriptor {
            profile_id: ServiceClassProfileIdentifier::AdvancedAudioDistribution,
            major_version: 1,
            minor_version: 2,
        }]),
        ..ServiceDefinition::new_empty()
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

// Highest AAC bitrate we want to transmit
const MAX_BITRATE_AAC: u32 = 250000;

/// Builds the set of streams which we currently support, streaming from the source_type given.
fn build_local_streams(source_type: AudioSourceType) -> avdtp::Result<stream::Streams> {
    // TODO(BT-533): detect codecs, add streams for each codec

    let source_task_builder = media_task::SourceTaskBuilder::new(source_type);
    let mut streams = stream::Streams::new();

    let sbc_codec_info = SbcCodecInfo::new(
        SbcSamplingFrequency::FREQ48000HZ,
        SbcChannelMode::JOINT_STEREO,
        SbcBlockCount::MANDATORY_SRC,
        SbcSubBands::MANDATORY_SRC,
        SbcAllocation::MANDATORY_SRC,
        SbcCodecInfo::BITPOOL_MIN,
        SbcCodecInfo::BITPOOL_MAX,
    )?;
    fx_vlog!(1, "Supported SBC codec parameters: {:?}.", sbc_codec_info);

    let sbc_endpoint = avdtp::StreamEndpoint::new(
        SBC_SEID,
        avdtp::MediaType::Audio,
        avdtp::EndpointType::Source,
        vec![
            avdtp::ServiceCapability::MediaTransport,
            avdtp::ServiceCapability::MediaCodec {
                media_type: avdtp::MediaType::Audio,
                codec_type: avdtp::MediaCodecType::AUDIO_SBC,
                codec_extra: sbc_codec_info.to_bytes().to_vec(),
            },
        ],
    )?;

    streams.insert(stream::Stream::build(sbc_endpoint, source_task_builder.clone()));
    fx_vlog!(1, "SBC Stream added at SEID {}", SBC_SEID);

    let aac_codec_info = AacCodecInfo::new(
        AacObjectType::MANDATORY_SRC,
        AacSamplingFrequency::FREQ48000HZ,
        AacChannels::TWO,
        true,
        MAX_BITRATE_AAC,
    )?;

    fx_vlog!(1, "Supported AAC codec parameters: {:?}.", aac_codec_info);

    let aac_endpoint = avdtp::StreamEndpoint::new(
        AAC_SEID,
        avdtp::MediaType::Audio,
        avdtp::EndpointType::Source,
        vec![
            avdtp::ServiceCapability::MediaTransport,
            avdtp::ServiceCapability::MediaCodec {
                media_type: avdtp::MediaType::Audio,
                codec_type: avdtp::MediaCodecType::AUDIO_AAC,
                codec_extra: aac_codec_info.to_bytes().to_vec(),
            },
        ],
    )?;

    streams.insert(stream::Stream::build(aac_endpoint, source_task_builder));
    fx_vlog!(1, "AAC stream added at SEID {}", AAC_SEID);

    Ok(streams)
}

struct Peers {
    peers: DetachableMap<PeerId, Peer>,
    streams: stream::Streams,
    profile: ProfileProxy,
}

impl Peers {
    fn new(streams: stream::Streams, profile: ProfileProxy) -> Self {
        Peers { peers: DetachableMap::new(), profile, streams }
    }

    pub(crate) fn get(&self, id: &PeerId) -> Option<Arc<peer::Peer>> {
        self.peers.get(id).and_then(|p| p.upgrade())
    }

    async fn discovered(&mut self, id: PeerId, desc: ProfileDescriptor) -> Result<(), Error> {
        if let Some(peer) = self.peers.get(&id) {
            if let Some(peer) = peer.upgrade() {
                if let None = peer.set_descriptor(desc.clone()) {
                    self.spawn_streaming(id);
                }
            }
            return Ok(());
        }
        let channel = match self
            .profile
            .connect(&mut id.into(), PSM_AVDTP, ChannelParameters::new_empty())
            .await?
        {
            Ok(channel) => channel,
            Err(code) => return Err(format_err!("Couldn't connect to peer {}: {:?}", id, code)),
        };

        match channel.socket {
            Some(socket) => self.connected(id, socket, Some(desc))?,
            None => fx_log_warn!("Couldn't connect {}: no socket", id),
        };
        Ok(())
    }

    fn connected(
        &mut self,
        id: PeerId,
        channel: zx::Socket,
        desc: Option<ProfileDescriptor>,
    ) -> Result<(), Error> {
        if let Some(peer) = self.peers.get(&id) {
            if let Some(peer) = peer.upgrade() {
                if let Err(e) = peer.receive_channel(channel) {
                    fx_log_warn!("{} connected an unexpected channel: {}", id, e);
                }
            }
        } else {
            let avdtp_peer =
                avdtp::Peer::new(channel).map_err(|e| avdtp::Error::ChannelSetup(e))?;
            let peer = Peer::create(id, avdtp_peer, self.streams.as_new(), self.profile.clone());
            // Start the streaming task if the profile information is populated.
            // Otherwise, `self.discovered()` will do so.
            let start_streaming_flag = desc.map_or(false, |d| {
                peer.set_descriptor(d);
                true
            });
            self.peers.insert(id, peer);

            if start_streaming_flag {
                self.spawn_streaming(id);
            }
        }
        Ok(())
    }

    fn spawn_streaming(&mut self, id: PeerId) {
        let weak_peer = self.peers.get(&id).expect("just added");
        fuchsia_async::spawn_local(async move {
            if let Err(e) = start_streaming(&weak_peer).await {
                fx_log_info!("Failed to stream: {:?}", e);
                weak_peer.detach();
            }
        });
    }
}

/// Pick a reasonable quality bitrate to use by default. 64k average per channel.
const PREFERRED_BITRATE_AAC: u32 = 128000;

/// Represents a chosen remote stream endpoint and negotiated codec and encoder settings
#[derive(Debug)]
struct SelectedStream<'a> {
    remote_stream: &'a avdtp::StreamEndpoint,
    codec_settings: avdtp::ServiceCapability,
    seid: u8,
}

impl<'a> SelectedStream<'a> {
    /// From the list of available remote streams, pick our preferred one and return matching codec parameters.
    fn pick(
        remote_streams: &'a Vec<avdtp::StreamEndpoint>,
    ) -> Result<SelectedStream<'_>, anyhow::Error> {
        // prefer AAC
        let (remote_stream, seid) =
            match Self::find_stream(remote_streams, &avdtp::MediaCodecType::AUDIO_AAC) {
                Some(aac_stream) => (aac_stream, AAC_SEID),
                None => (
                    Self::find_stream(remote_streams, &avdtp::MediaCodecType::AUDIO_SBC)
                        .ok_or(format_err!("Couldn't find a compatible stream"))?,
                    SBC_SEID,
                ),
            };

        let codec_settings = Self::negotiate_codec_settings(remote_stream)?;

        Ok(SelectedStream { remote_stream, codec_settings, seid })
    }

    /// From `stream` remote options, select our preferred and supported encoding options
    fn negotiate_codec_settings(
        stream: &'a avdtp::StreamEndpoint,
    ) -> Result<avdtp::ServiceCapability, Error> {
        match stream.codec_type() {
            Some(&avdtp::MediaCodecType::AUDIO_AAC) => {
                let codec_extra =
                    Self::lookup_codec_extra(stream).ok_or(format_err!("no codec extra found"))?;
                let remote_codec_info = AacCodecInfo::try_from(&codec_extra[..])?;
                let negotiated_bitrate =
                    std::cmp::min(remote_codec_info.bitrate(), PREFERRED_BITRATE_AAC);

                // Remote peers have to support these options
                let aac_codec_info = AacCodecInfo::new(
                    AacObjectType::MPEG2_AAC_LC,
                    AacSamplingFrequency::FREQ48000HZ,
                    AacChannels::TWO,
                    true,
                    negotiated_bitrate,
                )?;
                Ok(avdtp::ServiceCapability::MediaCodec {
                    media_type: avdtp::MediaType::Audio,
                    codec_type: avdtp::MediaCodecType::AUDIO_AAC,
                    codec_extra: aac_codec_info.to_bytes().to_vec(),
                })
            }
            Some(&avdtp::MediaCodecType::AUDIO_SBC) => {
                // TODO(39321): Choose codec options based on availability and quality.
                let sbc_codec_info = SbcCodecInfo::new(
                    SbcSamplingFrequency::FREQ48000HZ,
                    SbcChannelMode::JOINT_STEREO,
                    SbcBlockCount::SIXTEEN,
                    SbcSubBands::EIGHT,
                    SbcAllocation::LOUDNESS,
                    /* min_bpv= */ 53,
                    /* max_bpv= */ 53,
                )?;

                Ok(avdtp::ServiceCapability::MediaCodec {
                    media_type: avdtp::MediaType::Audio,
                    codec_type: avdtp::MediaCodecType::AUDIO_SBC,
                    codec_extra: sbc_codec_info.to_bytes().to_vec(),
                })
            }
            Some(t) => Err(format_err!("Unsupported codec {:?}", t)),
            None => Err(format_err!("No codec type")),
        }
    }

    fn find_stream(
        remote_streams: &'a Vec<avdtp::StreamEndpoint>,
        codec_type: &avdtp::MediaCodecType,
    ) -> Option<&'a avdtp::StreamEndpoint> {
        remote_streams
            .iter()
            .filter(|stream| stream.information().endpoint_type() == &avdtp::EndpointType::Sink)
            .find(|stream| stream.codec_type() == Some(codec_type))
    }

    fn lookup_codec_extra(stream: &'a avdtp::StreamEndpoint) -> Option<&'a Vec<u8>> {
        stream.capabilities().iter().find_map(|cap| match cap {
            avdtp::ServiceCapability::MediaCodec { codec_extra, .. } => Some(codec_extra),
            _ => None,
        })
    }
}

async fn start_streaming(peer: &DetachableWeak<PeerId, Peer>) -> Result<(), anyhow::Error> {
    let streams_fut = {
        let strong = peer.upgrade().ok_or(format_err!("Disconnected"))?;
        strong.collect_capabilities()
    };
    let remote_streams = streams_fut.await?;
    let selected_stream = SelectedStream::pick(&remote_streams)?;

    let strong = peer.upgrade().ok_or(format_err!("Disconnected"))?;
    strong
        .start_stream(
            selected_stream.seid.try_into()?,
            selected_stream.remote_stream.local_id().clone(),
            selected_stream.codec_settings.clone(),
        )
        .await
        .map_err(Into::into)
}

/// Defines the options available from the command line
#[derive(FromArgs)]
#[argh(description = "Bluetooth Advanced Audio Distribution Profile: Source")]
struct Opt {
    #[argh(option, default = "AudioSourceType::AudioOut")]
    /// audio source. options: [audio_out, big_ben]. Defaults to 'audio_out'
    source: AudioSourceType,
}

#[fasync::run_singlethreaded]
async fn main() -> Result<(), Error> {
    // Get the command line args.
    let opts: Opt = argh::from_env();

    fuchsia_syslog::init_with_tags(&["a2dp-source"]).expect("Can't init logger");
    fuchsia_trace_provider::trace_provider_create_with_fdio();

    let controller_pool = Arc::new(Mutex::new(AvdtpControllerPool::new()));

    let mut fs = ServiceFs::new();

    let mut lifecycle = ComponentLifecycleServer::spawn();
    fs.dir("svc").add_fidl_service(lifecycle.fidl_service());

    let pool_clone = controller_pool.clone();
    fs.dir("svc").add_fidl_service(move |s| pool_clone.lock().connected(s));
    if let Err(e) = fs.take_and_serve_directory_handle() {
        fx_log_warn!("Unable to serve Inspect service directory: {}", e);
    }

    fasync::spawn(fs.collect::<()>());

    let profile_svc = fuchsia_component::client::connect_to_service::<ProfileMarker>()
        .context("connecting to Bluetooth profile service")?;

    let service_defs = vec![make_profile_service_definition()];

    let (connect_client, connect_requests) =
        create_request_stream().context("ConnectionReceiver creation")?;

    profile_svc
        .advertise(
            &mut service_defs.into_iter(),
            SecurityRequirements::new_empty(),
            ChannelParameters::new_empty(),
            connect_client,
        )
        .context("advertising A2DP service")?;

    const ATTRS: [u16; 4] = [
        ATTR_PROTOCOL_DESCRIPTOR_LIST,
        ATTR_SERVICE_CLASS_ID_LIST,
        ATTR_BLUETOOTH_PROFILE_DESCRIPTOR_LIST,
        ATTR_A2DP_SUPPORTED_FEATURES,
    ];

    let (results_client, results_requests) =
        create_request_stream().context("SearchResults creation")?;

    profile_svc.search(ServiceClassProfileIdentifier::AudioSink, &ATTRS, results_client)?;

    let streams = build_local_streams(opts.source)?;
    let peers = Peers::new(streams, profile_svc);

    lifecycle.set(LifecycleState::Ready).await.expect("lifecycle server to set value");

    handle_profile_events(peers, controller_pool, connect_requests, results_requests).await
}

async fn handle_profile_events(
    mut peers: Peers,
    controller_pool: Arc<Mutex<AvdtpControllerPool>>,
    mut connect_requests: ConnectionReceiverRequestStream,
    mut results_requests: SearchResultsRequestStream,
) -> Result<(), Error> {
    loop {
        select! {
            connect_request = connect_requests.try_next() => {
                let connected = match connect_request? {
                    None => return Err(format_err!("BR/EDR ended service registration")),
                    Some(request) => request,
                };
                let ConnectionReceiverRequest::Connected { peer_id, channel, .. } = connected;
                let peer_id: PeerId = peer_id.into();
                fx_log_info!("Connected sink {}", peer_id);
                let socket = channel.socket.ok_or(format_err!("socket from profile should not be None"))?;
                if let Err(e) = peers.connected(peer_id, socket, None) {
                    fx_log_info!("Error connecting peer {}: {:?}", peer_id, e);
                    continue;
                }
                if let Some(peer) = peers.get(&peer_id) {
                    controller_pool.lock().peer_connected(peer_id, peer.avdtp_peer());
                }
            },
            results_request = results_requests.try_next() => {
                let result = match results_request? {
                    None => return Err(format_err!("BR/EDR ended service search")),
                    Some(request) => request,
                };
                let SearchResultsRequest::ServiceFound { peer_id, protocol, attributes, responder } = result;
                let peer_id: PeerId = peer_id.into();
                fx_log_info!(
                    "Discovered sink {} - protocol {:?}: {:?}",
                    peer_id,
                    protocol,
                    attributes
                );
                responder.send()?;
                let profile = match find_profile_descriptors(&attributes) {
                    Ok(profiles) => profiles.into_iter().next().expect("at least one profile descriptor"),
                    Err(_) => {
                        fx_log_info!("Couldn't find profile in peer {} search results, ignoring.", peer_id);
                        continue;
                    }
                };
                if let Err(e) = peers.discovered(peer_id, profile).await {
                    fx_log_info!("Error with discovered peer {}: {:?}", peer_id, e);
                }
            },
            complete => break,
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use fidl::endpoints::create_proxy_and_stream;
    use futures::{pin_mut, task::Poll};
    use matches::assert_matches;

    #[test]
    fn test_responds_to_search_results() {
        let mut exec = fasync::Executor::new().expect("executor should build");
        let (profile_proxy, _profile_stream) =
            create_proxy_and_stream::<ProfileMarker>().expect("Profile proxy should be created");
        let (results_proxy, results_stream) = create_proxy_and_stream::<SearchResultsMarker>()
            .expect("SearchResults proxy should be created");
        let (_connect_proxy, connect_stream) =
            create_proxy_and_stream::<ConnectionReceiverMarker>()
                .expect("ConnectionReceiver proxy should be created");

        let peers = Peers::new(stream::Streams::new(), profile_proxy);
        let controller_pool = Arc::new(Mutex::new(AvdtpControllerPool::new()));

        let handler_fut =
            handle_profile_events(peers, controller_pool, connect_stream, results_stream);
        pin_mut!(handler_fut);

        let res = exec.run_until_stalled(&mut handler_fut);
        assert!(res.is_pending());

        // Report a search result. This should be replied to.
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
    fn test_stream_selection() {
        // test that aac is preferred
        let remote_streams = vec![
            avdtp::StreamEndpoint::new(
                AAC_SEID,
                avdtp::MediaType::Audio,
                avdtp::EndpointType::Sink,
                vec![
                    avdtp::ServiceCapability::MediaTransport,
                    avdtp::ServiceCapability::MediaCodec {
                        media_type: avdtp::MediaType::Audio,
                        codec_type: avdtp::MediaCodecType::AUDIO_AAC,
                        codec_extra: vec![0, 0, 0, 0, 0, 0],
                    },
                ],
            )
            .expect("stream endpoint"),
            avdtp::StreamEndpoint::new(
                SBC_SEID,
                avdtp::MediaType::Audio,
                avdtp::EndpointType::Sink,
                vec![
                    avdtp::ServiceCapability::MediaTransport,
                    avdtp::ServiceCapability::MediaCodec {
                        media_type: avdtp::MediaType::Audio,
                        codec_type: avdtp::MediaCodecType::AUDIO_SBC,
                        codec_extra: vec![0, 0, 0, 0],
                    },
                ],
            )
            .expect("stream endpoint"),
        ];

        assert_matches!(
            SelectedStream::pick(&remote_streams),
            Ok(SelectedStream {
                seid: AAC_SEID,
                codec_settings:
                    avdtp::ServiceCapability::MediaCodec {
                        media_type: avdtp::MediaType::Audio,
                        codec_type: avdtp::MediaCodecType::AUDIO_AAC,
                        ..
                    },
                ..
            })
        );

        // only sbc available, should return sbc
        let remote_streams = vec![avdtp::StreamEndpoint::new(
            SBC_SEID,
            avdtp::MediaType::Audio,
            avdtp::EndpointType::Sink,
            vec![
                avdtp::ServiceCapability::MediaTransport,
                avdtp::ServiceCapability::MediaCodec {
                    media_type: avdtp::MediaType::Audio,
                    codec_type: avdtp::MediaCodecType::AUDIO_SBC,
                    codec_extra: vec![0, 0, 0, 0],
                },
            ],
        )
        .expect("stream endpoint")];

        assert_matches!(
            SelectedStream::pick(&remote_streams),
            Ok(SelectedStream {
                seid: SBC_SEID,
                codec_settings:
                    avdtp::ServiceCapability::MediaCodec {
                        media_type: avdtp::MediaType::Audio,
                        codec_type: avdtp::MediaCodecType::AUDIO_SBC,
                        ..
                    },
                ..
            })
        );

        // none available, should error
        let remote_streams = vec![];
        assert_matches!(SelectedStream::pick(&remote_streams), Err(Error { .. }));
    }
}
