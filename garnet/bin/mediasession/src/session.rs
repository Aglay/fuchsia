// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::log_error::log_error_discard_result;
use crate::service::ServiceEvent;
use crate::{Result, CHANNEL_BUFFER_SIZE};
use failure::ResultExt;
use fidl::endpoints::ServerEnd;
use fidl_fuchsia_media::TimelineFunction;
use fidl_fuchsia_mediasession::{
    Error, PlaybackCapabilities, PlaybackState, PlaybackStatus, SessionControlHandle, SessionEvent,
    SessionEventStream, SessionMarker, SessionProxy, SessionRequest,
};
use fuchsia_async as fasync;
use fuchsia_zircon as zx;
use futures::{
    channel::mpsc::{channel, Receiver, Sender},
    future, select, Future, FutureExt, SinkExt, StreamExt, TryFutureExt, TryStreamExt,
};

/// `Session` multiplexes the `fuchsia.mediasession.Session` implementation of
/// a published media session.
pub struct Session {
    id: zx::Koid,
    event_broadcaster: EventBroadcaster,
    request_forwarder: RequestForwarder,
    service_event_sink: Sender<ServiceEvent>,
}

impl Session {
    /// Creates a new `Session` which multiplexes `controller_proxy`.
    ///
    /// `Session` should not be used after it sends a `SessionClosed`
    /// `ServiceEvent`.
    pub fn new(
        id: zx::Koid,
        controller_proxy: SessionProxy,
        service_event_sink: Sender<ServiceEvent>,
    ) -> Result<Self> {
        let event_stream = controller_proxy.take_event_stream();
        Ok(Self {
            id,
            event_broadcaster: EventBroadcaster::new(id, service_event_sink.clone(), event_stream),
            request_forwarder: RequestForwarder::new(controller_proxy),
            service_event_sink,
        })
    }

    pub fn id(&self) -> zx::Koid {
        self.id
    }

    pub async fn serve(self, mut fidl_requests: Receiver<ServerEnd<SessionMarker>>) {
        let (forwarder_handle, forwarder_registration) = future::AbortHandle::new_pair();
        let request_sink = self.request_forwarder.start(forwarder_registration);
        let mut service_event_sink = self.service_event_sink;
        let session_id = self.id;
        let mut listener_sink = self.event_broadcaster.start(
            async move {
                forwarder_handle.abort();
                trylog!(await!(service_event_sink.send(ServiceEvent::SessionClosed(session_id)))
                    .context("Sending Session epitaph."));
            },
        );

        // Connect all new clients to the request forwarding and event broadcasting tasks.
        while let Some(new_client) = await!(fidl_requests.next()) {
            let request_sink = request_sink.clone();
            let (request_stream, control_handle) = trylog!(new_client
                .into_stream_and_control_handle()
                .context("Converting client to request stream."));

            // Send the control handle to the event broadcasting task.
            trylog!(await!(listener_sink.send(control_handle)));

            // Forward the request stream to the request serving task.
            fasync::spawn(
                request_stream
                    .filter_map(|result| {
                        future::ready(match result {
                            Ok(event) => Some(Ok(event)),
                            Err(_) => None,
                        })
                    })
                    .forward(request_sink)
                    .map(log_error_discard_result),
            );
        }
    }
}

/// Forwards requests from all proxied clients to the backing
/// `fuchsia.mediasession.Session` implementation.
struct RequestForwarder {
    controller_proxy: SessionProxy,
}

impl RequestForwarder {
    fn new(controller_proxy: SessionProxy) -> Self {
        Self { controller_proxy }
    }

    fn start(self, abort_registration: future::AbortRegistration) -> Sender<SessionRequest> {
        let (request_sink, request_stream) = channel(CHANNEL_BUFFER_SIZE);
        let serve_fut = future::Abortable::new(self.serve(request_stream), abort_registration)
            .unwrap_or_else(|_| {
                // This is just an abort message; ignore it.
                // This happens when the backing implementation disconnects and is not an error.
            });
        fasync::spawn(serve_fut);
        request_sink
    }

    async fn serve(self, mut request_stream: Receiver<SessionRequest>) {
        while let Some(request) = await!(request_stream.next()) {
            trylog!(self.serve_request_with_controller(request));
        }
    }

    /// Forwards a single request to the `SessionProxy`.
    fn serve_request_with_controller(&self, request: SessionRequest) -> Result<()> {
        match request {
            SessionRequest::Play { .. } => self.controller_proxy.play()?,
            SessionRequest::Pause { .. } => self.controller_proxy.pause()?,
            SessionRequest::Stop { .. } => self.controller_proxy.stop()?,
            SessionRequest::SeekToPosition { position, .. } => {
                self.controller_proxy.seek_to_position(position)?
            }
            SessionRequest::SkipForward { skip_amount, .. } => {
                self.controller_proxy.skip_forward(skip_amount)?
            }
            SessionRequest::SkipReverse { skip_amount, .. } => {
                self.controller_proxy.skip_reverse(skip_amount)?
            }
            SessionRequest::NextItem { .. } => self.controller_proxy.next_item()?,
            SessionRequest::PrevItem { .. } => self.controller_proxy.prev_item()?,
            SessionRequest::SetPlaybackRate { playback_rate, .. } => {
                self.controller_proxy.set_playback_rate(playback_rate)?
            }
            SessionRequest::SetRepeatMode { repeat_mode, .. } => {
                self.controller_proxy.set_repeat_mode(repeat_mode)?
            }
            SessionRequest::SetShuffleMode { shuffle_on, .. } => {
                self.controller_proxy.set_shuffle_mode(shuffle_on)?
            }
            SessionRequest::BindGainControl { gain_control_request, .. } => {
                self.controller_proxy.bind_gain_control(gain_control_request)?
            }
            SessionRequest::ConnectToExtension { extension, channel, .. } => {
                self.controller_proxy.connect_to_extension(&extension, channel)?
            }
        };
        Ok(())
    }
}

/// Stores the most recent events sent by the backing
/// `fuchsia.mediasession.Session` implementation which represent the state of
/// the media session.
#[derive(Default)]
struct SessionState {
    metadata: Option<SessionEvent>,
    playback_capabilities: Option<SessionEvent>,
    playback_status: Option<SessionEvent>,
}

impl SessionState {
    fn new() -> Self {
        Default::default()
    }

    /// Updates the stored state with the new `event`.
    fn update(&mut self, event: SessionEvent) {
        let to_update = match &event {
            SessionEvent::OnPlaybackStatusChanged { .. } => &mut self.playback_status,
            SessionEvent::OnMetadataChanged { .. } => &mut self.metadata,
            SessionEvent::OnPlaybackCapabilitiesChanged { .. } => &mut self.playback_capabilities,
        };
        *to_update = Some(event);
    }
}

/// Broadcasts events from the backing `Session` implementation to all proxied
/// clients and, if it is qualifying activity, to the Media Session service so it
/// can track active sessions.
struct EventBroadcaster {
    id: zx::Koid,
    service_event_sink: Sender<ServiceEvent>,
    source: SessionEventStream,
}

impl EventBroadcaster {
    fn new(
        id: zx::Koid,
        service_event_sink: Sender<ServiceEvent>,
        source: SessionEventStream,
    ) -> Self {
        Self { id, service_event_sink, source }
    }

    fn start(
        self,
        epitaph: impl Future<Output = ()> + Send + 'static,
    ) -> Sender<SessionControlHandle> {
        let (listener_sink, listener_stream) = channel(CHANNEL_BUFFER_SIZE);
        fasync::spawn(
            async move {
                await!(self.serve(listener_stream));
                await!(epitaph);
            },
        );
        listener_sink
    }

    /// Continuously broadcasts events from the `source` to listeners
    /// received over the `listener_stream`.
    ///
    /// New listeners are pushed the most recent state from before their
    /// connection.
    ///
    /// Event listeners are dropped as their client ends disconnect.
    async fn serve(mut self, mut listener_stream: Receiver<SessionControlHandle>) {
        let mut session_state = SessionState::new();
        let mut listeners = Vec::new();

        loop {
            select! {
                event = self.source.try_next() => {
                    let event = trylogbreak!(event);
                    match event {
                        Some(mut event) => {
                            if Self::event_is_an_active_playback_status(&event) {
                                trylogbreak!(await!(self.service_event_sink.send(ServiceEvent::SessionActivity(self.id))));
                            }
                            Self::broadcast_event(&mut event, &mut listeners);
                            session_state.update(event);
                        },
                        None => break,
                    };
                }
                listener = listener_stream.next() => {
                    match listener.map(|listener| {
                        Self::deliver_state(&mut session_state, &listener).map(|_| listener)
                    }) {
                        Some(Ok(listener)) => listeners.push(listener),
                        Some(Err(e)) => eprintln!("Failed to push state to new event listener: {}.", e),
                        _ => (),
                    }
                }
                complete => {
                    break
                },
            }
        }

        for listener in listeners.into_iter() {
            listener.shutdown();
        }
    }

    /// Broadcasts an event to all event listeners by control handle.
    ///
    /// Only connected event listeners are retained.
    ///
    /// This will not work for events which have handles as they can only be sent
    /// once.
    fn broadcast_event(event: &mut SessionEvent, listeners: &mut Vec<SessionControlHandle>) {
        listeners.retain(|listener| Self::send_event(listener, event).is_ok());
    }

    /// Sends an event to a listener by a control handle.
    fn send_event(listener: &SessionControlHandle, event: &mut SessionEvent) -> Result<()> {
        // TODO(turnage): remove this field by field copy for a `.clone()` invocation
        //                when FIDL structs and tables get a method for that.
        match event {
            SessionEvent::OnPlaybackStatusChanged { playback_status } => listener
                .send_on_playback_status_changed(PlaybackStatus {
                    duration: playback_status.duration.clone(),
                    playback_state: playback_status.playback_state.clone(),
                    playback_function: playback_status.playback_function.as_ref().map(|function| {
                        TimelineFunction {
                            subject_time: function.subject_time,
                            reference_time: function.reference_time,
                            subject_delta: function.subject_delta,
                            reference_delta: function.reference_delta,
                        }
                    }),
                    repeat_mode: playback_status.repeat_mode.clone(),
                    shuffle_on: playback_status.shuffle_on,
                    has_next_item: playback_status.has_next_item,
                    has_prev_item: playback_status.has_prev_item,
                    error: playback_status.error.as_ref().map(|error| Error {
                        code: error.code,
                        description: error.description.clone(),
                    }),
                }),
            SessionEvent::OnMetadataChanged { media_metadata } => {
                listener.send_on_metadata_changed(media_metadata)
            }
            SessionEvent::OnPlaybackCapabilitiesChanged { playback_capabilities } => listener
                .send_on_playback_capabilities_changed(PlaybackCapabilities {
                    can_play: playback_capabilities.can_play,
                    can_stop: playback_capabilities.can_stop,
                    can_pause: playback_capabilities.can_pause,
                    can_seek_to_position: playback_capabilities.can_seek_to_position,
                    can_skip_forward: playback_capabilities.can_skip_forward,
                    can_skip_reverse: playback_capabilities.can_skip_reverse,
                    supported_skip_intervals: playback_capabilities
                        .supported_skip_intervals
                        .clone(),
                    supported_playback_rates: playback_capabilities
                        .supported_playback_rates
                        .clone(),
                    can_shuffle: playback_capabilities.can_shuffle,
                    supported_repeat_modes: playback_capabilities.supported_repeat_modes.clone(),
                    can_change_to_next_item: playback_capabilities.can_change_to_next_item,
                    can_change_to_prev_item: playback_capabilities.can_change_to_prev_item,
                    custom_extensions: playback_capabilities.custom_extensions.clone(),
                    has_gain_control: playback_capabilities.has_gain_control,
                }),
        }
        .map_err(Into::into)
    }

    /// Delivers `state` to `listener`.
    fn deliver_state(state: &mut SessionState, listener: &SessionControlHandle) -> Result<()> {
        [&mut state.metadata, &mut state.playback_capabilities, &mut state.playback_status]
            .iter_mut()
            .filter_map(|update: &mut &mut Option<SessionEvent>| (*update).as_mut())
            .map(|update: &mut SessionEvent| Self::send_event(&listener, update))
            .collect()
    }

    fn event_is_an_active_playback_status(event: &SessionEvent) -> bool {
        match *event {
            SessionEvent::OnPlaybackStatusChanged { ref playback_status }
                if playback_status.playback_state == Some(PlaybackState::Playing) =>
            {
                true
            }
            _ => false,
        }
    }
}
