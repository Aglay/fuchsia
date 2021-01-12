// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::{
    container::ComponentIdentity,
    logs::{
        buffer::AccountedBuffer,
        error::StreamError,
        socket::{Encoding, LogMessageSocket},
        stats::LogStreamStats,
        Message,
    },
};
use fidl::endpoints::RequestStream;
use fidl_fuchsia_diagnostics::{Interest, Selector};
use fidl_fuchsia_logger::{
    LogInterestSelector, LogSinkControlHandle, LogSinkRequest, LogSinkRequestStream,
};
use fuchsia_async::Task;
use futures::{channel::mpsc, prelude::*};
use parking_lot::Mutex;
use std::sync::Arc;
use tracing::{error, warn};

pub struct LogsArtifactsContainer {
    /// The source of logs in this container.
    pub identity: Arc<ComponentIdentity>,

    /// Current interest for this component.
    interest: Mutex<Interest>,

    /// Control handles for connected clients.
    control_handles: Mutex<Vec<LogSinkControlHandle>>,

    /// Inspect instrumentation.
    pub stats: Arc<LogStreamStats>,

    /// Buffer for all log messages.
    buffer: Arc<Mutex<AccountedBuffer<Message>>>,
}

impl LogsArtifactsContainer {
    pub fn new(
        identity: Arc<ComponentIdentity>,
        interest_selectors: &[LogInterestSelector],
        stats: LogStreamStats,
        buffer: Arc<Mutex<AccountedBuffer<Message>>>,
    ) -> Self {
        let new = Self {
            buffer,
            identity,
            control_handles: Mutex::new(vec![]),
            interest: Mutex::new(Interest::EMPTY),
            stats: Arc::new(stats),
        };

        // there are no control handles so this won't notify anyone
        new.update_interest(interest_selectors);

        new
    }

    /// Handle `LogSink` protocol on `stream`. This function does not return until the channel is
    /// closed. Each socket received from the `LogSink` client is drained by a `Task` which is sent
    /// on `sender`. The `Task`s do not complete until their sockets have been closed.
    ///
    /// Sends an `OnRegisterInterest` message right away so producers know someone is listening.
    /// We send `Interest::EMPTY` unless a different interest has previously been specified for
    /// this component.
    pub async fn handle_log_sink(
        self: Arc<Self>,
        mut stream: LogSinkRequestStream,
        sender: mpsc::UnboundedSender<Task<()>>,
    ) {
        let control = stream.control_handle();
        control.send_on_register_interest(self.interest.lock().clone()).ok();
        self.control_handles.lock().push(control);

        macro_rules! handle_socket {
            ($ctor:ident($socket:ident, $control_handle:ident)) => {{
                match LogMessageSocket::$ctor($socket, self.identity.clone(), self.stats.clone()) {
                    Ok(log_stream) => {
                        let task = Task::spawn(self.clone().drain_messages(log_stream));
                        sender.unbounded_send(task).expect("channel alive for whole program");
                    }
                    Err(e) => {
                        $control_handle.shutdown();
                        warn!(?self.identity, %e, "error creating socket")
                    }
                };
            }}
        }

        while let Some(next) = stream.next().await {
            match next {
                Ok(LogSinkRequest::Connect { socket, control_handle }) => {
                    handle_socket! {new(socket, control_handle)};
                }
                Ok(LogSinkRequest::ConnectStructured { socket, control_handle }) => {
                    handle_socket! {new_structured(socket, control_handle)};
                }
                Err(e) => error!(?self.identity, %e, "error handling log sink"),
            }
        }
    }

    /// Drain a `LogMessageSocket` which wraps a socket from a component
    /// generating logs.
    pub async fn drain_messages<E>(self: Arc<Self>, mut log_stream: LogMessageSocket<E>)
    where
        E: Encoding + Unpin,
    {
        loop {
            match log_stream.next().await {
                Ok(message) => {
                    self.ingest_message(message);
                }
                Err(StreamError::Closed) => return,
                Err(e) => {
                    warn!(source = %self.identity.relative_moniker.join("/"), %e, "closing socket");
                    return;
                }
            }
        }
    }

    /// Updates log stats in inspect and push the message onto the container's buffer.
    pub fn ingest_message(&self, message: Message) {
        self.stats.ingest_message(&message);
        self.buffer.lock().push(message);
    }

    /// Set the `Interest` for this component, calling `LogSink/OnRegisterInterest` with all
    /// control handles if it is a change from the previous interest.
    pub fn update_interest(&self, interest_selectors: &[LogInterestSelector]) {
        let mut new_interest = Interest::EMPTY;
        for selector in interest_selectors {
            // TODO(fxbug.dev/66997) matching api for ComponentSelector from selectors crate
            let to_match = Arc::new(Selector {
                component_selector: Some(selector.selector.clone()),
                ..Selector::EMPTY
            });
            if selectors::match_component_moniker_against_selector(
                &self.identity.relative_moniker,
                &to_match,
            )
            .unwrap_or_default()
            {
                new_interest = selector.interest.clone();
            }
        }

        let mut current_interest = self.interest.lock();
        if *current_interest != new_interest {
            self.control_handles
                .lock()
                .retain(|handle| handle.send_on_register_interest(new_interest.clone()).is_ok());
            *current_interest = new_interest;
        }
    }
}
