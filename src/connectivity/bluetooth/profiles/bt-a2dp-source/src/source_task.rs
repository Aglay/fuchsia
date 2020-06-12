// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{format_err, Error},
    bt_a2dp::{codec::MediaCodecConfig, inspect::DataStreamInspect, media_task::*},
    bt_avdtp::MediaStream,
    fidl_fuchsia_media::{AudioChannelId, AudioPcmMode, PcmFormat},
    fuchsia_async as fasync,
    fuchsia_bluetooth::types::PeerId,
    fuchsia_syslog::{self, fx_log_info, fx_log_warn, fx_vlog},
    fuchsia_trace as trace,
    futures::{
        future::{AbortHandle, Abortable},
        lock::Mutex,
        AsyncWriteExt, TryStreamExt,
    },
    std::sync::Arc,
};

use crate::encoding::{EncodedStream, RtpPacketBuilder};
use crate::sources;

/// SourceTaskBuilder is a MediaTaskBuilder will build `ConfiguredSourceTask`s when configured.
/// `source_type` determines where the source of audio is provided.
/// When configured, a test stream is created to confirm that it is possible to stream audio using
/// the configuration.  This stream is discarded and the stream is restarted when the resulting
/// `ConfiguredSourceTask` is started.
#[derive(Clone)]
pub struct SourceTaskBuilder {
    /// The type of source audio.
    source_type: sources::AudioSourceType,
}

impl MediaTaskBuilder for SourceTaskBuilder {
    fn configure(
        &self,
        peer_id: &PeerId,
        codec_config: &MediaCodecConfig,
        data_stream_inspect: DataStreamInspect,
    ) -> Result<Box<dyn MediaTask>, Error> {
        // all sinks must support these options
        let pcm_format = PcmFormat {
            pcm_mode: AudioPcmMode::Linear,
            bits_per_sample: 16,
            frames_per_second: 48000,
            channel_map: vec![AudioChannelId::Lf, AudioChannelId::Rf],
        };

        let source_stream = sources::build_stream(&peer_id, pcm_format.clone(), self.source_type)?;
        if let Err(e) = EncodedStream::build(pcm_format.clone(), source_stream, codec_config) {
            fx_vlog!(2, "SourceTaskBuilder: can't build encoded stream: {:?}", e);
            return Err(e);
        }
        Ok(Box::new(ConfiguredSourceTask::build(
            pcm_format,
            self.source_type,
            peer_id.clone(),
            codec_config,
            data_stream_inspect,
        )))
    }
}

impl SourceTaskBuilder {
    /// Make a new builder that will source audio from `source_type`.  See `sources::build_stream`
    /// for documentation on the types of streams that are available.
    pub fn new(source_type: sources::AudioSourceType) -> Self {
        Self { source_type }
    }
}

/// Provides audio from this to the MediaStream when started.  Streams are created and started when
/// this task is started, and destoyed when stopped.
struct ConfiguredSourceTask {
    /// The type of source audio.
    source_type: sources::AudioSourceType,
    /// Format the source audio should be produced in.
    pcm_format: PcmFormat,
    /// Id of the peer that will be receiving the stream.  Used to distinguish sources for Fuchsia
    /// Media.
    peer_id: PeerId,
    /// Configuration providing the format of encoded audio requested by the peer.
    codec_config: MediaCodecConfig,
    /// Handle used to stop the streaming task when stopped.
    stop_sender: Option<AbortHandle>,
    /// Data Stream inspect object for tracking total bytes / current transfer speed.
    data_stream_inspect: Arc<Mutex<DataStreamInspect>>,
}

impl ConfiguredSourceTask {
    /// The main streaming task. Reads encoded audio from the encoded_stream and packages into RTP
    /// packets, sending the resulting RTP packets using `media_stream`.
    async fn stream_task(
        codec_config: MediaCodecConfig,
        mut encoded_stream: EncodedStream,
        mut media_stream: MediaStream,
        data_stream_inspect: Arc<Mutex<DataStreamInspect>>,
    ) -> Result<(), Error> {
        let frames_per_encoded = codec_config.pcm_frames_per_encoded_frame() as u32;
        let mut packet_builder = RtpPacketBuilder::new(
            codec_config.frames_per_packet() as u8,
            codec_config.rtp_frame_header().to_vec(),
        );
        loop {
            let encoded = match encoded_stream.try_next().await? {
                None => continue,
                Some(encoded) => encoded,
            };
            let packet = match packet_builder.push_frame(encoded, frames_per_encoded)? {
                None => continue,
                Some(packet) => packet,
            };

            trace::duration_begin!("bt-a2dp-source", "Media:PacketSent");
            if let Err(e) = media_stream.write(&packet).await {
                fx_log_info!("Failed sending packet to peer: {}", e);
                let _ = data_stream_inspect.try_lock().map(|mut l| {
                    l.record_transferred(packet.len(), fasync::Time::now());
                });
                trace::duration_end!("bt-a2dp-source", "Media:PacketSent");
                return Ok(());
            }
            trace::duration_end!("bt-a2dp-source", "Media:PacketSent");
        }
    }

    /// Build a new ConfiguredSourceTask.  Usually only called by SourceTaskBuilder.
    /// `ConfiguredSourceTask::start` will only return errors if the settings here can not produce a
    /// stream.  No checks are done when building.
    fn build(
        pcm_format: PcmFormat,
        source_type: sources::AudioSourceType,
        peer_id: PeerId,
        codec_config: &MediaCodecConfig,
        data_stream_inspect: DataStreamInspect,
    ) -> Self {
        Self {
            pcm_format,
            source_type,
            peer_id,
            codec_config: codec_config.clone(),
            stop_sender: None,
            data_stream_inspect: Arc::new(Mutex::new(data_stream_inspect)),
        }
    }
}

impl MediaTask for ConfiguredSourceTask {
    fn start(&mut self, stream: MediaStream) -> Result<(), Error> {
        if self.stop_sender.is_some() {
            return Err(format_err!("Already started, can't start again"));
        }
        let source_stream =
            sources::build_stream(&self.peer_id, self.pcm_format.clone(), self.source_type)?;
        let encoded_stream =
            EncodedStream::build(self.pcm_format.clone(), source_stream, &self.codec_config)?;
        let inspect = self.data_stream_inspect.clone();
        let stream_fut =
            Self::stream_task(self.codec_config.clone(), encoded_stream, stream, inspect);
        let _ = self.data_stream_inspect.try_lock().map(|mut l| l.start());
        let (stop_handle, stop_registration) = AbortHandle::new_pair();
        let stream_fut = Abortable::new(stream_fut, stop_registration);
        fasync::spawn(async move {
            trace::instant!("bt-a2dp-source", "Media:Start", trace::Scope::Thread);
            match stream_fut.await {
                Err(_) | Ok(Ok(())) => {}
                Ok(Err(e)) => fx_log_warn!("ConfiguredSourceTask ended with error: {:?}", e),
            };
        });
        self.stop_sender = Some(stop_handle);
        Ok(())
    }

    fn stop(&mut self) -> Result<(), Error> {
        trace::instant!("bt-a2dp-source", "Media:Stop", trace::Scope::Thread);
        self.stop_sender.take().map(|x| x.abort());
        Ok(())
    }
}

impl Drop for ConfiguredSourceTask {
    fn drop(&mut self) {
        let _ = self.stop();
    }
}
