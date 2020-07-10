// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::{format_err, Context as _, Error};
use fidl::endpoints::{ClientEnd, ServerEnd, ServiceMarker};
use fidl_fuchsia_logger::LogSinkMarker;
use fidl_fuchsia_logger::{
    LogFilterOptions, LogInterestSelector, LogListenerSafeMarker, LogRequest, LogRequestStream,
    LogSinkControlHandle, LogSinkRequest, LogSinkRequestStream,
};
use fidl_fuchsia_sys2 as fsys;
use fidl_fuchsia_sys_internal::{
    LogConnection, LogConnectionListenerRequest, LogConnectorProxy, SourceIdentity,
};
use fuchsia_async as fasync;
use fuchsia_inspect as inspect;
use fuchsia_inspect_derive::Inspect;
use fuchsia_zircon as zx;
use futures::{channel::mpsc, future::FutureObj, lock::Mutex, FutureExt, StreamExt, TryStreamExt};
use log::{error, warn};
use std::sync::Arc;

mod buffer;
mod debuglog;
mod error;
mod interest;
mod listener;
pub mod message;
mod socket;
mod stats;

pub use debuglog::{convert_debuglog_to_log_message, KernelDebugLog};
use interest::InterestDispatcher;
use listener::{pool::Pool, pretend_scary_listener_is_safe, Listener};
use message::Message;
use socket::{Encoding, LogMessageSocket};
use stats::LogSource;

/// Store 4 MB of log messages and delete on FIFO basis.
const OLD_MSGS_BUF_SIZE: usize = 4 * 1024 * 1024;

/// The `LogManager` is responsible for brokering all logging in the archivist.
#[derive(Clone, Inspect)]
pub struct LogManager {
    #[inspect(forward)]
    inner: Arc<Mutex<ManagerInner>>,
}

#[derive(Inspect)]
struct ManagerInner {
    #[inspect(skip)]
    listeners: Pool,
    #[inspect(skip)]
    interest_dispatcher: InterestDispatcher,
    #[inspect(rename = "buffer_stats")]
    log_msg_buffer: buffer::MemoryBoundedBuffer<Message>,
    stats: stats::LogManagerStats,
    inspect_node: inspect::Node,
}

impl LogManager {
    pub fn new() -> Self {
        Self {
            inner: Arc::new(Mutex::new(ManagerInner {
                listeners: Pool::default(),
                interest_dispatcher: InterestDispatcher::default(),
                log_msg_buffer: buffer::MemoryBoundedBuffer::new(OLD_MSGS_BUF_SIZE),
                stats: stats::LogManagerStats::new_detached(),
                inspect_node: inspect::Node::default(),
            })),
        }
    }

    /// Drain the kernel's debug log. The returned future completes once
    /// existing messages have been ingested.
    pub async fn drain_debuglog<K>(self, klog_reader: K)
    where
        K: debuglog::DebugLog + Send + Sync + 'static,
    {
        let mut source = SourceIdentity::empty();
        source.component_name = Some("(klog)".to_string());
        source.component_url = Some("fuchsia-boot://klog".to_string());
        let component_log_stats = {
            let inner = self.inner.lock().await;
            inner.stats.get_component_log_stats(&source).await
        };
        let mut kernel_logger = debuglog::DebugLogBridge::create(klog_reader);
        let messages = match kernel_logger.existing_logs().await {
            Ok(messages) => messages,
            Err(e) => {
                error!("failed to read from kernel log, important logs may be missing: {}", e);
                return;
            }
        };
        for message in messages {
            component_log_stats.lock().await.record_log(&message);
            self.ingest_message(message, LogSource::Kernel).await;
        }

        let res = kernel_logger
            .listen()
            .try_for_each(|message| {
                async {
                    component_log_stats.clone().lock().await.record_log(&message);
                    self.ingest_message(message, LogSource::Kernel).await
                }
                .map(Ok)
            })
            .await;
        if let Err(e) = res {
            error!("failed to drain kernel log, important logs may be missing: {}", e);
        }
    }

    /// Drain log sink for messages sent by the archivist itself.
    pub async fn drain_internal_log_sink(self, socket: zx::Socket, name: &str) {
        // TODO(50105): Figure out how to properly populate SourceIdentity
        let mut source = SourceIdentity::empty();
        source.component_name = Some(name.to_owned());
        let source = Arc::new(source);
        let log_stream = LogMessageSocket::new(socket, source)
            .expect("failed to create internal LogMessageSocket");
        self.drain_messages(log_stream).await;
        unreachable!();
    }

    /// Handle `LogConnectionListener` for the parent realm, eventually passing
    /// `LogSink` connections into the manager.
    pub async fn handle_log_connector(
        self,
        connector: LogConnectorProxy,
        sender: mpsc::UnboundedSender<FutureObj<'static, ()>>,
    ) {
        match connector.take_log_connection_listener().await {
            Ok(Some(listener)) => {
                let mut connections =
                    listener.into_stream().expect("getting request stream from server end");
                while let Ok(Some(connection)) = connections.try_next().await {
                    match connection {
                        LogConnectionListenerRequest::OnNewConnection {
                            connection: LogConnection { log_request, source_identity },
                            control_handle: _,
                        } => {
                            let stream = log_request
                                .into_stream()
                                .expect("getting LogSinkRequestStream from serverend");
                            let source = Arc::new(source_identity);
                            fasync::spawn(self.clone().handle_log_sink(
                                stream,
                                source,
                                sender.clone(),
                            ))
                        }
                    };
                }
            }
            Ok(None) => warn!("local realm already gave out LogConnectionListener, skipping logs"),
            Err(e) => error!("error retrieving LogConnectionListener from LogConnector: {}", e),
        }
    }

    /// Handle `LogSink` protocol on `stream`. The future returned by this
    /// function will not complete before all messages on this connection are
    /// processed.
    pub async fn handle_log_sink(
        self,
        mut stream: LogSinkRequestStream,
        source: Arc<SourceIdentity>,
        sender: mpsc::UnboundedSender<FutureObj<'static, ()>>,
    ) {
        if source.component_name.is_none() {
            self.inner.lock().await.stats.record_unattributed();
        }

        while let Some(next) = stream.next().await {
            match next {
                Ok(LogSinkRequest::Connect { socket, control_handle }) => {
                    match LogMessageSocket::new(socket, source.clone())
                        .context("creating log stream from socket")
                    {
                        Ok(log_stream) => {
                            self.add_interest_listener(&source, control_handle).await;
                            let fut =
                                FutureObj::new(Box::new(self.clone().drain_messages(log_stream)));
                            let res = sender.unbounded_send(fut);
                            if let Err(e) = res {
                                warn!("error queuing log message drain: {}", e);
                            }
                        }
                        Err(e) => {
                            control_handle.shutdown();
                            warn!("error creating socket from {:?}: {}", source, e)
                        }
                    };
                }
                Ok(LogSinkRequest::ConnectStructured { socket, control_handle }) => {
                    match LogMessageSocket::new_structured(socket, source.clone())
                        .context("creating log stream from socket")
                    {
                        Ok(log_stream) => {
                            self.add_interest_listener(&source, control_handle).await;
                            let fut =
                                FutureObj::new(Box::new(self.clone().drain_messages(log_stream)));
                            let res = sender.unbounded_send(fut);
                            if let Err(e) = res {
                                warn!("error queuing log message drain: {}", e);
                            }
                        }
                        Err(e) => {
                            control_handle.shutdown();
                            warn!("error creating socket from {:?}: {}", source, e)
                        }
                    };
                }
                Err(e) => error!("error handling log sink from {:?}: {}", source, e),
            }
        }
    }

    /// Drain a `LogMessageSocket` which wraps a socket from a component
    /// generating logs.
    async fn drain_messages<E>(self, mut log_stream: LogMessageSocket<E>)
    where
        E: Encoding + Unpin,
    {
        let component_log_stats = {
            let inner = self.inner.lock().await;
            inner.stats.get_component_log_stats(log_stream.source()).await
        };
        while let Some(next) = log_stream.next().await {
            match next {
                Ok(message) => {
                    component_log_stats.lock().await.record_log(&message);
                    self.ingest_message(message, stats::LogSource::LogSink).await;
                }
                Err(e) => {
                    self.inner.lock().await.stats.record_closed_stream();
                    warn!("closing socket from {:?}: {}", log_stream.source(), e);
                    return;
                }
            }
        }
    }

    /// Add 'Interest' listener to connect the interest dispatcher to the
    /// LogSinkControlHandle (weak reference) associated with the given source.
    async fn add_interest_listener(
        &self,
        source: &Arc<SourceIdentity>,
        control_handle: LogSinkControlHandle,
    ) {
        let event_listener = Arc::new(control_handle);
        self.inner
            .lock()
            .await
            .interest_dispatcher
            .add_interest_listener(source, Arc::downgrade(&event_listener));
    }

    /// Spawn a task to handle requests from components reading the shared log.
    pub async fn handle_log(self, stream: LogRequestStream) {
        if let Err(e) = self.handle_log_requests(stream).await {
            warn!("error handling Log requests: {}", e);
        }
    }

    /// Handle the components v2 EventStream for attributed logs of v2
    /// components.
    pub async fn handle_event_stream(
        self,
        mut stream: fsys::EventStreamRequestStream,
        sender: mpsc::UnboundedSender<FutureObj<'static, ()>>,
    ) {
        while let Ok(Some(request)) = stream.try_next().await {
            match request {
                fsys::EventStreamRequest::OnEvent { event, .. } => {
                    if let Err(e) = self.handle_event(event, sender.clone()) {
                        error!("Unable to process event: {}", e);
                    }
                }
            }
        }
    }

    /// Handle the components v2 CapabilityRequested event for attributed logs of
    /// v2 components.
    fn handle_event(
        &self,
        event: fsys::Event,
        sender: mpsc::UnboundedSender<FutureObj<'static, ()>>,
    ) -> Result<(), Error> {
        let identity = Self::source_identity_from_event(&event)?;
        let stream = Self::log_sink_request_stream_from_event(event)?;
        fasync::spawn(self.clone().handle_log_sink(stream, identity, sender));
        Ok(())
    }

    /// Extract the SourceIdentity from a components v2 event.
    // TODO(fxb/54330): LogManager should have its own error type.
    fn source_identity_from_event(event: &fsys::Event) -> Result<Arc<SourceIdentity>, Error> {
        let target_moniker = event
            .descriptor
            .as_ref()
            .and_then(|descriptor| descriptor.moniker.clone())
            .ok_or(format_err!("No moniker present"))?;

        let component_url = event
            .descriptor
            .as_ref()
            .and_then(|descriptor| descriptor.component_url.clone())
            .ok_or(format_err!("No URL present"))?;

        let mut source = SourceIdentity::empty();
        source.component_url = Some(component_url.clone());
        source.component_name = Some(target_moniker.clone());
        Ok(Arc::new(source))
    }

    /// Extract the LogSinkRequestStream from a CapabilityRequested v2 event.
    // TODO(fxb/54330): LogManager should have its own error type.
    fn log_sink_request_stream_from_event(
        event: fsys::Event,
    ) -> Result<LogSinkRequestStream, Error> {
        let payload =
            event.event_result.ok_or(format_err!("Missing event result.")).and_then(|result| {
                match result {
                    fsys::EventResult::Payload(fsys::EventPayload::CapabilityRequested(
                        payload,
                    )) => Ok(payload),
                    fsys::EventResult::Error(fsys::EventError {
                        description: Some(description),
                        ..
                    }) => Err(format_err!("{}", description)),
                    _ => Err(format_err!("Incorrect event type.")),
                }
            })?;

        let capability_path = payload.path.ok_or(format_err!("Missing capability path"))?;
        if capability_path != format!("/svc/{}", LogSinkMarker::NAME) {
            return Err(format_err!("Incorrect LogSink capability_path {}", capability_path));
        }
        let capability = payload.capability.ok_or(format_err!("Missing capability"))?;
        let server_end = ServerEnd::<LogSinkMarker>::new(capability);
        server_end.into_stream().map_err(|_| format_err!("Unable to create LogSinkRequestStream"))
    }

    /// Handle requests to `fuchsia.logger.Log`. All request types read the
    /// whole backlog from memory, `DumpLogs(Safe)` stops listening after that.
    async fn handle_log_requests(self, mut stream: LogRequestStream) -> Result<(), Error> {
        while let Some(request) = stream.try_next().await? {
            let (listener, options, dump_logs, selectors) = match request {
                LogRequest::ListenSafe { log_listener, options, .. } => {
                    (log_listener, options, false, None)
                }
                LogRequest::DumpLogsSafe { log_listener, options, .. } => {
                    (log_listener, options, true, None)
                }

                LogRequest::ListenSafeWithSelectors {
                    log_listener, options, selectors, ..
                } => (log_listener, options, false, Some(selectors)),

                // TODO(fxb/48758) delete these methods!
                LogRequest::Listen { log_listener, options, .. } => {
                    warn!("Use of fuchsia.logger.Log.Listen. Use ListenSafe.");
                    let listener = pretend_scary_listener_is_safe(log_listener)?;
                    (listener, options, false, None)
                }
                LogRequest::DumpLogs { log_listener, options, .. } => {
                    warn!("Use of fuchsia.logger.Log.DumpLogs. Use DumpLogsSafe.");
                    let listener = pretend_scary_listener_is_safe(log_listener)?;
                    (listener, options, true, None)
                }
            };

            self.handle_log_listener(listener, options, dump_logs, selectors).await?;
        }
        Ok(())
    }

    /// Handle a new listener, sending it all cached messages and either calling
    /// `Done` if `dump_logs` is true or adding it to the pool of ongoing
    /// listeners if not.
    async fn handle_log_listener(
        &self,
        log_listener: ClientEnd<LogListenerSafeMarker>,
        options: Option<Box<LogFilterOptions>>,
        dump_logs: bool,
        selectors: Option<Vec<LogInterestSelector>>,
    ) -> Result<(), Error> {
        let mut listener = Listener::new(log_listener, options)?;

        let mut inner = self.inner.lock().await;

        listener.backfill(inner.log_msg_buffer.iter()).await;

        if !listener.is_healthy() {
            warn!("listener dropped before we finished");
            return Ok(());
        }

        if dump_logs {
            listener.done();
        } else {
            if let Some(s) = selectors {
                inner.interest_dispatcher.update_selectors(s).await;
            }
            inner.listeners.add(listener);
        }
        Ok(())
    }

    /// Ingest an individual log message.
    async fn ingest_message(&self, log_msg: Message, source: stats::LogSource) {
        let mut inner = self.inner.lock().await;

        // We always record the log before sending messages to listeners because
        // we want to be able to see that stats are updated as soon as we receive
        // messages in tests.
        inner.stats.record_log(&log_msg, source);
        inner.listeners.send(&log_msg).await;
        inner.log_msg_buffer.push(log_msg);
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        crate::logs::debuglog::tests::{TestDebugEntry, TestDebugLog},
        crate::logs::message::{fx_log_packet_t, Severity, MAX_DATAGRAM_LEN},
        diagnostic_streams::{
            encode::Encoder, Argument, Record, Severity as StreamSeverity, Value,
        },
        fidl_fuchsia_logger::{
            LogFilterOptions, LogLevelFilter, LogMarker, LogMessage, LogProxy, LogSinkMarker,
            LogSinkProxy,
        },
        fidl_fuchsia_sys2 as fsys,
        fuchsia_inspect::assert_inspect_tree,
        fuchsia_inspect_derive::WithInspect,
        fuchsia_zircon as zx,
        matches::assert_matches,
        std::{io::Cursor, marker::PhantomData},
        validating_log_listener::{validate_log_dump, validate_log_stream},
    };

    #[fasync::run_singlethreaded(test)]
    async fn test_log_manager_simple() {
        TestHarness::new().manager_test(false).await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_log_manager_dump() {
        TestHarness::new().manager_test(true).await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn unfiltered_stats() {
        let first_packet = setup_default_packet();
        let first_message = LogMessage {
            pid: first_packet.metadata.pid,
            tid: first_packet.metadata.tid,
            time: first_packet.metadata.time,
            dropped_logs: first_packet.metadata.dropped_logs,
            severity: first_packet.metadata.severity,
            msg: String::from("BBBBB"),
            tags: vec![String::from("AAAAA")],
        };

        let (mut second_packet, mut second_message) = (first_packet.clone(), first_message.clone());
        second_packet.metadata.pid = 0;
        second_message.pid = second_packet.metadata.pid;

        let (mut third_packet, mut third_message) = (second_packet.clone(), second_message.clone());
        third_packet.metadata.severity = LogLevelFilter::Info.into_primitive().into();
        third_message.severity = third_packet.metadata.severity;

        let (fourth_packet, fourth_message) = (third_packet.clone(), third_message.clone());

        let (mut fifth_packet, mut fifth_message) = (fourth_packet.clone(), fourth_message.clone());
        fifth_packet.metadata.severity = LogLevelFilter::Error.into_primitive().into();
        fifth_message.severity = fifth_packet.metadata.severity;

        let harness = TestHarness::new();
        let mut stream = harness.create_stream(Arc::new(SourceIdentity::empty()));
        stream.write_packets(vec![
            first_packet,
            second_packet,
            third_packet,
            fourth_packet,
            fifth_packet,
        ]);
        let log_stats_tree = harness
            .filter_test(
                vec![first_message, second_message, third_message, fourth_message, fifth_message],
                None,
            )
            .await;

        assert_inspect_tree!(
            log_stats_tree,
            root: {
                log_stats: {
                    total_logs: 5u64,
                    kernel_logs: 0u64,
                    logsink_logs: 5u64,
                    trace_logs: 0u64,
                    debug_logs: 0u64,
                    info_logs: 2u64,
                    warning_logs: 2u64,
                    error_logs: 1u64,
                    fatal_logs: 0u64,
                    closed_streams: 0u64,
                    unattributed_log_sinks: 1u64,
                    by_component: { "(unattributed)": {
                        total_logs: 5u64,
                        trace_logs: 0u64,
                        debug_logs: 0u64,
                        info_logs: 2u64,
                        warning_logs: 2u64,
                        error_logs: 1u64,
                        fatal_logs: 0u64,
                    } },
                    buffer_stats: {
                        rolled_out_entries: 0u64,
                    }
                },
            }
        );
    }

    async fn attributed_inspect_two_streams_different_identities_by_reader(
        harness: TestHarness,
        log_reader1: Arc<dyn LogReader>,
        log_reader2: Arc<dyn LogReader>,
    ) {
        let mut packet = setup_default_packet();
        let message = LogMessage {
            pid: packet.metadata.pid,
            tid: packet.metadata.tid,
            time: packet.metadata.time,
            dropped_logs: packet.metadata.dropped_logs,
            severity: packet.metadata.severity,
            msg: String::from("BBBBB"),
            tags: vec![String::from("AAAAA")],
        };

        let mut packet2 = packet.clone();
        packet2.metadata.severity = LogLevelFilter::Error.into_primitive().into();
        let mut message2 = message.clone();
        message2.severity = packet2.metadata.severity;

        let mut foo_stream = harness.create_stream_from_log_reader(log_reader1.clone());
        foo_stream.write_packet(&mut packet);

        let mut bar_stream = harness.create_stream_from_log_reader(log_reader2.clone());
        bar_stream.write_packet(&mut packet2);
        let log_stats_tree = harness.filter_test(vec![message, message2], None).await;

        assert_inspect_tree!(
            log_stats_tree,
            root: {
                log_stats: {
                    total_logs: 2u64,
                    kernel_logs: 0u64,
                    logsink_logs: 2u64,
                    trace_logs: 0u64,
                    debug_logs: 0u64,
                    info_logs: 0u64,
                    warning_logs: 1u64,
                    error_logs: 1u64,
                    fatal_logs: 0u64,
                    closed_streams: 0u64,
                    unattributed_log_sinks: 0u64,
                    by_component: {
                        "http://foo.com": {
                            total_logs: 1u64,
                            trace_logs: 0u64,
                            debug_logs: 0u64,
                            info_logs: 0u64,
                            warning_logs: 1u64,
                            error_logs: 0u64,
                            fatal_logs: 0u64,
                        },
                        "http://bar.com": {
                            total_logs: 1u64,
                            trace_logs: 0u64,
                            debug_logs: 0u64,
                            info_logs: 0u64,
                            warning_logs: 0u64,
                            error_logs: 1u64,
                            fatal_logs: 0u64,
                        }
                    },
                    buffer_stats: {
                        rolled_out_entries: 0u64,
                    }
                },
            }
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn attributed_inspect_two_streams_different_identities() {
        let harness = TestHarness::new();

        let identity = Arc::new(SourceIdentity {
            component_name: Some("foo".into()),
            component_url: Some("http://foo.com".into()),
            instance_id: None,
            realm_path: None,
        });
        let log_reader1 = DefaultLogReader::new(harness.log_manager.clone(), identity);

        let identity = Arc::new(SourceIdentity {
            component_name: Some("bar".into()),
            component_url: Some("http://bar.com".into()),
            instance_id: None,
            realm_path: None,
        });
        let log_reader2 = DefaultLogReader::new(harness.log_manager.clone(), identity);

        attributed_inspect_two_streams_different_identities_by_reader(
            harness,
            log_reader1,
            log_reader2,
        )
        .await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn attributed_inspect_two_v2_streams_different_identities() {
        let harness = TestHarness::new();

        let log_reader1 =
            EventStreamLogReader::new(harness.log_manager.clone(), "foo", "http://foo.com");
        let log_reader2 =
            EventStreamLogReader::new(harness.log_manager.clone(), "bar", "http://bar.com");
        attributed_inspect_two_streams_different_identities_by_reader(
            harness,
            log_reader1,
            log_reader2,
        )
        .await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn attributed_inspect_two_mixed_streams_different_identities() {
        let harness = TestHarness::new();

        let log_reader1 =
            EventStreamLogReader::new(harness.log_manager.clone(), "foo", "http://foo.com");
        let identity = Arc::new(SourceIdentity {
            component_name: Some("bar".into()),
            component_url: Some("http://bar.com".into()),
            instance_id: None,
            realm_path: None,
        });
        let log_reader2 = DefaultLogReader::new(harness.log_manager.clone(), identity);
        attributed_inspect_two_streams_different_identities_by_reader(
            harness,
            log_reader1,
            log_reader2,
        )
        .await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn source_identity_from_v2_event() {
        let target_moniker = "foo".to_string();
        let target_url = "http://foo.com".to_string();
        let (_log_sink_proxy, log_sink_server_end) =
            fidl::endpoints::create_proxy::<LogSinkMarker>().unwrap();
        let event = create_capability_requested_event(
            target_moniker.clone(),
            target_url.clone(),
            log_sink_server_end.into_channel(),
        );
        let identity = LogManager::source_identity_from_event(&event).unwrap();
        assert_matches!(&identity.component_name,
                        Some(component_name) if *component_name == target_moniker);
        assert_matches!(&identity.component_url,
                        Some(component_url) if *component_url == target_url);
    }

    #[fasync::run_singlethreaded(test)]
    async fn log_sink_request_stream_from_v2_event() {
        let target_moniker = "foo".to_string();
        let target_url = "http://foo.com".to_string();
        let (_log_sink_proxy, log_sink_server_end) =
            fidl::endpoints::create_proxy::<LogSinkMarker>().unwrap();
        let event = create_capability_requested_event(
            target_moniker.clone(),
            target_url.clone(),
            log_sink_server_end.into_channel(),
        );
        LogManager::log_sink_request_stream_from_event(event).unwrap();
    }

    async fn attributed_inspect_two_streams_same_identity_by_reader(
        harness: TestHarness,
        log_reader1: Arc<dyn LogReader>,
        log_reader2: Arc<dyn LogReader>,
    ) {
        let mut packet = setup_default_packet();
        let message = LogMessage {
            pid: packet.metadata.pid,
            tid: packet.metadata.tid,
            time: packet.metadata.time,
            dropped_logs: packet.metadata.dropped_logs,
            severity: packet.metadata.severity,
            msg: String::from("BBBBB"),
            tags: vec![String::from("AAAAA")],
        };

        let mut packet2 = packet.clone();
        packet2.metadata.severity = LogLevelFilter::Error.into_primitive().into();
        let mut message2 = message.clone();
        message2.severity = packet2.metadata.severity;

        let mut foo_stream = harness.create_stream_from_log_reader(log_reader1.clone());
        foo_stream.write_packet(&mut packet);

        let mut bar_stream = harness.create_stream_from_log_reader(log_reader2.clone());
        bar_stream.write_packet(&mut packet2);
        let log_stats_tree = harness.filter_test(vec![message, message2], None).await;

        assert_inspect_tree!(
            log_stats_tree,
            root: {
                log_stats: {
                    total_logs: 2u64,
                    kernel_logs: 0u64,
                    logsink_logs: 2u64,
                    trace_logs: 0u64,
                    debug_logs: 0u64,
                    info_logs: 0u64,
                    warning_logs: 1u64,
                    error_logs: 1u64,
                    fatal_logs: 0u64,
                    closed_streams: 0u64,
                    unattributed_log_sinks: 0u64,
                    by_component: {
                        "http://foo.com": {
                            total_logs: 2u64,
                            trace_logs: 0u64,
                            debug_logs: 0u64,
                            info_logs: 0u64,
                            warning_logs: 1u64,
                            error_logs: 1u64,
                            fatal_logs: 0u64,
                        },
                    },
                    buffer_stats: {
                        rolled_out_entries: 0u64,
                    }
                },
            }
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn attributed_inspect_two_streams_same_identity() {
        let harness = TestHarness::new();
        let identity = Arc::new(SourceIdentity {
            component_name: Some("foo".into()),
            component_url: Some("http://foo.com".into()),
            instance_id: None,
            realm_path: None,
        });
        let log_reader = DefaultLogReader::new(harness.log_manager.clone(), identity);
        attributed_inspect_two_streams_same_identity_by_reader(
            harness,
            log_reader.clone(),
            log_reader.clone(),
        )
        .await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn attributed_inspect_two_v2_streams_same_identity() {
        let harness = TestHarness::new();
        let log_reader =
            EventStreamLogReader::new(harness.log_manager.clone(), "foo", "http://foo.com");
        attributed_inspect_two_streams_same_identity_by_reader(
            harness,
            log_reader.clone(),
            log_reader.clone(),
        )
        .await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn attributed_inspect_two_mixed_streams_same_identity() {
        let harness = TestHarness::new();

        let log_reader1 =
            EventStreamLogReader::new(harness.log_manager.clone(), "foo", "http://foo.com");
        let identity = Arc::new(SourceIdentity {
            component_name: Some("foo".into()),
            component_url: Some("http://foo.com".into()),
            instance_id: None,
            realm_path: None,
        });
        let log_reader2 = DefaultLogReader::new(harness.log_manager.clone(), identity);
        attributed_inspect_two_streams_same_identity_by_reader(harness, log_reader1, log_reader2)
            .await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_filter_by_pid() {
        let p = setup_default_packet();
        let mut p2 = p.clone();
        p2.metadata.pid = 0;
        let lm = LogMessage {
            pid: p.metadata.pid,
            tid: p.metadata.tid,
            time: p.metadata.time,
            dropped_logs: p.metadata.dropped_logs,
            severity: p.metadata.severity,
            msg: String::from("BBBBB"),
            tags: vec![String::from("AAAAA")],
        };
        let options = LogFilterOptions {
            filter_by_pid: true,
            pid: 1,
            filter_by_tid: false,
            tid: 0,
            min_severity: LogLevelFilter::None,
            verbosity: 0,
            tags: vec![],
        };

        let harness = TestHarness::new();
        let mut stream = harness.create_stream(Arc::new(SourceIdentity::empty()));
        stream.write_packets(vec![p, p2]);
        harness.filter_test(vec![lm], Some(options)).await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_filter_by_tid() {
        let mut p = setup_default_packet();
        p.metadata.pid = 0;
        let mut p2 = p.clone();
        p2.metadata.tid = 0;
        let lm = LogMessage {
            pid: p.metadata.pid,
            tid: p.metadata.tid,
            time: p.metadata.time,
            dropped_logs: p.metadata.dropped_logs,
            severity: p.metadata.severity,
            msg: String::from("BBBBB"),
            tags: vec![String::from("AAAAA")],
        };
        let options = LogFilterOptions {
            filter_by_pid: false,
            pid: 1,
            filter_by_tid: true,
            tid: 1,
            min_severity: LogLevelFilter::None,
            verbosity: 0,
            tags: vec![],
        };

        let harness = TestHarness::new();
        let mut stream = harness.create_stream(Arc::new(SourceIdentity::empty()));
        stream.write_packets(vec![p, p2]);
        harness.filter_test(vec![lm], Some(options)).await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_filter_by_min_severity() {
        let p = setup_default_packet();
        let mut p2 = p.clone();
        p2.metadata.pid = 0;
        p2.metadata.tid = 0;
        p2.metadata.severity = LogLevelFilter::Error.into_primitive().into();
        let mut p3 = p.clone();
        p3.metadata.severity = LogLevelFilter::Info.into_primitive().into();
        let mut p4 = p.clone();
        p4.metadata.severity = 0x70; // custom
        let mut p5 = p.clone();
        p5.metadata.severity = LogLevelFilter::Fatal.into_primitive().into();
        let lm = LogMessage {
            pid: p2.metadata.pid,
            tid: p2.metadata.tid,
            time: p2.metadata.time,
            dropped_logs: p2.metadata.dropped_logs,
            severity: p2.metadata.severity,
            msg: String::from("BBBBB"),
            tags: vec![String::from("AAAAA")],
        };
        let options = LogFilterOptions {
            filter_by_pid: false,
            pid: 1,
            filter_by_tid: false,
            tid: 1,
            min_severity: LogLevelFilter::Error,
            verbosity: 0,
            tags: vec![],
        };

        let harness = TestHarness::new();
        let mut stream = harness.create_stream(Arc::new(SourceIdentity::empty()));
        stream.write_packets(vec![p, p2, p3, p4, p5]);
        harness.filter_test(vec![lm], Some(options)).await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_filter_by_combination() {
        let mut p = setup_default_packet();
        p.metadata.pid = 0;
        p.metadata.tid = 0;
        let mut p2 = p.clone();
        p2.metadata.severity = LogLevelFilter::Error.into_primitive().into();
        let mut p3 = p.clone();
        p3.metadata.pid = 1;
        let lm = LogMessage {
            pid: p2.metadata.pid,
            tid: p2.metadata.tid,
            time: p2.metadata.time,
            dropped_logs: p2.metadata.dropped_logs,
            severity: p2.metadata.severity,
            msg: String::from("BBBBB"),
            tags: vec![String::from("AAAAA")],
        };
        let options = LogFilterOptions {
            filter_by_pid: true,
            pid: 0,
            filter_by_tid: false,
            tid: 1,
            min_severity: LogLevelFilter::Error,
            verbosity: 0,
            tags: vec![],
        };

        let harness = TestHarness::new();
        let mut stream = harness.create_stream(Arc::new(SourceIdentity::empty()));
        stream.write_packets(vec![p, p2, p3]);
        harness.filter_test(vec![lm], Some(options)).await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_filter_by_tags() {
        let mut p = setup_default_packet();
        let mut p2 = p.clone();
        // p tags - "DDDDD"
        memset(&mut p.data[..], 1, 68, 5);

        p2.metadata.pid = 0;
        p2.metadata.tid = 0;
        p2.data[6] = 5;
        // p2 tag - "AAAAA", "BBBBB"
        // p2 msg - "CCCCC"
        memset(&mut p2.data[..], 13, 67, 5);

        let lm1 = LogMessage {
            pid: p.metadata.pid,
            tid: p.metadata.tid,
            time: p.metadata.time,
            dropped_logs: p.metadata.dropped_logs,
            severity: p.metadata.severity,
            msg: String::from("BBBBB"),
            tags: vec![String::from("DDDDD")],
        };
        let lm2 = LogMessage {
            pid: p2.metadata.pid,
            tid: p2.metadata.tid,
            time: p2.metadata.time,
            dropped_logs: p2.metadata.dropped_logs,
            severity: p2.metadata.severity,
            msg: String::from("CCCCC"),
            tags: vec![String::from("AAAAA"), String::from("BBBBB")],
        };
        let options = LogFilterOptions {
            filter_by_pid: false,
            pid: 1,
            filter_by_tid: false,
            tid: 1,
            min_severity: LogLevelFilter::None,
            verbosity: 0,
            tags: vec![String::from("BBBBB"), String::from("DDDDD")],
        };

        let harness = TestHarness::new();
        let mut stream = harness.create_stream(Arc::new(SourceIdentity::empty()));
        stream.write_packets(vec![p, p2]);
        harness.filter_test(vec![lm1, lm2], Some(options)).await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_structured_log() {
        let logs = vec![
            Record {
                timestamp: 6,
                severity: StreamSeverity::Info,
                arguments: vec![Argument {
                    name: "__msg".to_string(),
                    value: Value::Text("hi".to_string()),
                }],
            },
            Record { timestamp: 14, severity: StreamSeverity::Error, arguments: vec![] },
            Record {
                timestamp: 19,
                severity: StreamSeverity::Warn,
                arguments: vec![
                    Argument { name: String::from("__pid"), value: Value::UnsignedInt(0x1d1) },
                    Argument { name: String::from("__tid"), value: Value::UnsignedInt(0x1d2) },
                    Argument { name: String::from("__dropped"), value: Value::UnsignedInt(23) },
                    Argument {
                        name: String::from("__tag"),
                        value: Value::Text(String::from("tag")),
                    },
                    Argument {
                        name: String::from("__msg"),
                        value: Value::Text(String::from("message")),
                    },
                ],
            },
            Record {
                timestamp: 21,
                severity: StreamSeverity::Warn,
                arguments: vec![
                    Argument {
                        name: String::from("__tag"),
                        value: Value::Text(String::from("tag-1")),
                    },
                    Argument {
                        name: String::from("__tag"),
                        value: Value::Text(String::from("tag-2")),
                    },
                ],
            },
        ];

        let expected_logs = vec![
            LogMessage {
                pid: zx::sys::ZX_KOID_INVALID,
                tid: zx::sys::ZX_KOID_INVALID,
                time: 6,
                severity: Severity::Info.for_listener(),
                dropped_logs: 0,
                msg: String::from("hi"),
                tags: vec![],
            },
            LogMessage {
                pid: zx::sys::ZX_KOID_INVALID,
                tid: zx::sys::ZX_KOID_INVALID,
                time: 14,
                severity: Severity::Error.for_listener(),
                dropped_logs: 0,
                msg: String::from(""),
                tags: vec![],
            },
            LogMessage {
                pid: 0x1d1,
                tid: 0x1d2,
                time: 19,
                severity: Severity::Warn.for_listener(),
                dropped_logs: 23,
                msg: String::from("message"),
                tags: vec![String::from("tag")],
            },
            LogMessage {
                pid: zx::sys::ZX_KOID_INVALID,
                tid: zx::sys::ZX_KOID_INVALID,
                time: 21,
                severity: Severity::Warn.for_listener(),
                dropped_logs: 0,
                msg: String::from(""),
                tags: vec![String::from("tag-1"), String::from("tag-2")],
            },
        ];
        let harness = TestHarness::new();
        let mut stream = harness.create_structured_stream(Arc::new(SourceIdentity::empty()));
        stream.write_packets(logs);
        harness.filter_test(expected_logs, None).await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_debuglog_drainer() {
        let log1 = TestDebugEntry::new("log1".as_bytes());
        let log2 = TestDebugEntry::new("log2".as_bytes());
        let log3 = TestDebugEntry::new("log3".as_bytes());

        let klog_reader = TestDebugLog::new();
        klog_reader.enqueue_read_entry(&log1);
        klog_reader.enqueue_read_entry(&log2);
        // logs recieved after kernel indicates no logs should be read
        klog_reader.enqueue_read_fail(zx::Status::SHOULD_WAIT);
        klog_reader.enqueue_read_entry(&log3);
        klog_reader.enqueue_read_fail(zx::Status::SHOULD_WAIT);

        let expected_logs = vec![
            LogMessage {
                pid: log1.pid,
                tid: log1.tid,
                time: log1.timestamp,
                dropped_logs: 0,
                severity: fidl_fuchsia_logger::LogLevelFilter::Info as i32,
                msg: String::from("log1"),
                tags: vec![String::from("klog")],
            },
            LogMessage {
                pid: log2.pid,
                tid: log2.tid,
                time: log2.timestamp,
                dropped_logs: 0,
                severity: fidl_fuchsia_logger::LogLevelFilter::Info as i32,
                msg: String::from("log2"),
                tags: vec![String::from("klog")],
            },
            LogMessage {
                pid: log3.pid,
                tid: log3.tid,
                time: log3.timestamp,
                dropped_logs: 0,
                severity: fidl_fuchsia_logger::LogLevelFilter::Info as i32,
                msg: String::from("log3"),
                tags: vec![String::from("klog")],
            },
        ];

        let klog_stats_tree = debuglog_test(expected_logs, klog_reader).await;
        assert_inspect_tree!(
            klog_stats_tree,
            root: {
                log_stats: contains {
                    total_logs: 3u64,
                    kernel_logs: 3u64,
                    logsink_logs: 0u64,
                    trace_logs: 0u64,
                    debug_logs: 0u64,
                    info_logs: 3u64,
                    warning_logs: 0u64,
                    error_logs: 0u64,
                    fatal_logs: 0u64,
                    closed_streams: 0u64,
                    unattributed_log_sinks: 0u64,
                    by_component: {
                        "fuchsia-boot://klog": {
                            total_logs: 3u64,
                            trace_logs: 0u64,
                            debug_logs: 0u64,
                            info_logs: 3u64,
                            warning_logs: 0u64,
                            error_logs: 0u64,
                            fatal_logs: 0u64,
                        },
                    }
                }
            }
        );
    }

    struct TestHarness {
        inspector: inspect::Inspector,
        log_manager: LogManager,
        log_proxy: LogProxy,
    }

    fn create_capability_requested_event(
        target_moniker: String,
        target_url: String,
        capability: zx::Channel,
    ) -> fsys::Event {
        fsys::Event {
            event_type: Some(fsys::EventType::CapabilityRequested),
            descriptor: Some(fsys::ComponentDescriptor {
                moniker: Some(target_moniker),
                component_url: Some(target_url),
            }),
            event_result: Some(fsys::EventResult::Payload(
                fsys::EventPayload::CapabilityRequested(fsys::CapabilityRequestedPayload {
                    path: Some(format!("/svc/{}", LogSinkMarker::NAME)),
                    capability: Some(capability),
                }),
            )),
            ..fsys::Event::empty()
        }
    }

    impl TestHarness {
        fn new() -> Self {
            let inner = Arc::new(Mutex::new(ManagerInner {
                listeners: Pool::default(),
                interest_dispatcher: InterestDispatcher::default(),
                log_msg_buffer: buffer::MemoryBoundedBuffer::new(OLD_MSGS_BUF_SIZE),
                stats: stats::LogManagerStats::new_detached(),
                inspect_node: inspect::Node::default(),
            }));

            let inspector = inspect::Inspector::new();
            let log_manager =
                LogManager { inner }.with_inspect(inspector.root(), "log_stats").unwrap();

            let (log_proxy, log_stream) =
                fidl::endpoints::create_proxy_and_stream::<LogMarker>().unwrap();
            fasync::spawn(log_manager.clone().handle_log(log_stream));

            Self { inspector, log_manager, log_proxy }
        }

        /// Run a filter test, returning the Inspector to check Inspect output.
        async fn filter_test(
            self,
            expected: impl IntoIterator<Item = LogMessage>,
            filter_options: Option<LogFilterOptions>,
        ) -> inspect::Inspector {
            validate_log_stream(expected, self.log_proxy, filter_options).await;
            self.inspector
        }

        async fn manager_test(self, test_dump_logs: bool) {
            let mut p = setup_default_packet();
            let mut stream = self.create_stream(Arc::new(SourceIdentity::empty()));
            stream.write_packet(&mut p);

            p.metadata.severity = LogLevelFilter::Info.into_primitive().into();
            stream.write_packet(&mut p);

            let mut lm1 = LogMessage {
                time: p.metadata.time,
                pid: p.metadata.pid,
                tid: p.metadata.tid,
                dropped_logs: p.metadata.dropped_logs,
                severity: p.metadata.severity,
                msg: String::from("BBBBB"),
                tags: vec![String::from("AAAAA")],
            };
            let lm2 = copy_log_message(&lm1);
            lm1.severity = LogLevelFilter::Warn.into_primitive().into();
            let mut lm3 = copy_log_message(&lm2);
            lm3.pid = 2;

            p.metadata.pid = 2;
            stream.write_packet(&mut p);

            if test_dump_logs {
                validate_log_dump(vec![lm1, lm2, lm3], self.log_proxy, None).await;
            } else {
                validate_log_stream(vec![lm1, lm2, lm3], self.log_proxy, None).await;
            }
        }

        fn create_stream(&self, identity: Arc<SourceIdentity>) -> TestStream<LogPacketWriter> {
            let log_reader = DefaultLogReader::new(self.log_manager.clone(), identity);
            TestStream::new(log_reader)
        }

        fn create_stream_from_log_reader(
            &self,
            log_reader: Arc<dyn LogReader>,
        ) -> TestStream<LogPacketWriter> {
            TestStream::new(log_reader)
        }

        fn create_structured_stream(
            &self,
            identity: Arc<SourceIdentity>,
        ) -> TestStream<StructuredMessageWriter> {
            let log_reader = DefaultLogReader::new(self.log_manager.clone(), identity);
            TestStream::new(log_reader)
        }
    }

    /// A `LogWriter` can connect to and send `Packets` to a LogSink over a socket.
    trait LogWriter {
        type Packet;
        fn connect(log_sink: &LogSinkProxy, sout: zx::Socket);

        fn write(sout: &zx::Socket, packet: &Self::Packet);
    }

    /// A `LogWriter` that writes `fx_log_packet_t` to a LogSink in the syslog
    /// format.
    struct LogPacketWriter;

    /// A `LogWriter` that writes `Record` to a LogSink in the structured
    /// log format.
    struct StructuredMessageWriter;

    impl LogWriter for LogPacketWriter {
        type Packet = fx_log_packet_t;

        fn connect(log_sink: &LogSinkProxy, sout: zx::Socket) {
            log_sink.connect(sout).expect("unable to connect out socket to log sink");
        }

        fn write(sin: &zx::Socket, packet: &fx_log_packet_t) {
            sin.write(to_u8_slice(packet)).unwrap();
        }
    }

    impl LogWriter for StructuredMessageWriter {
        type Packet = Record;

        fn connect(log_sink: &LogSinkProxy, sin: zx::Socket) {
            log_sink.connect_structured(sin).expect("unable to connect out socket to log sink");
        }

        fn write(sin: &zx::Socket, record: &Record) {
            let mut buffer = Cursor::new(vec![0; MAX_DATAGRAM_LEN]);
            let mut encoder = Encoder::new(&mut buffer);
            encoder.write_record(record).unwrap();
            let slice = buffer.get_ref().as_slice();
            sin.write(slice).unwrap();
        }
    }

    /// A `LogReader` host a LogSink connection.
    trait LogReader {
        fn handle_request(
            &self,
            sender: mpsc::UnboundedSender<FutureObj<'static, ()>>,
        ) -> LogSinkProxy;
    }

    // A LogReader that exercises the handle_log_sink code path.
    struct DefaultLogReader {
        log_manager: LogManager,
        identity: Arc<SourceIdentity>,
    }

    impl DefaultLogReader {
        fn new(log_manager: LogManager, identity: Arc<SourceIdentity>) -> Arc<dyn LogReader> {
            Arc::new(Self { log_manager, identity })
        }
    }

    impl LogReader for DefaultLogReader {
        fn handle_request(
            &self,
            log_sender: mpsc::UnboundedSender<FutureObj<'static, ()>>,
        ) -> LogSinkProxy {
            let (log_sink_proxy, log_sink_stream) =
                fidl::endpoints::create_proxy_and_stream::<LogSinkMarker>().unwrap();
            fasync::spawn(self.log_manager.clone().handle_log_sink(
                log_sink_stream,
                self.identity.clone(),
                log_sender,
            ));
            log_sink_proxy
        }
    }

    // A LogReader that exercises the components v2 EventStream and CapabilityRequested event
    // code path for log attribution.
    struct EventStreamLogReader {
        log_manager: LogManager,
        target_moniker: String,
        target_url: String,
    }

    impl EventStreamLogReader {
        pub fn new(
            log_manager: LogManager,
            target_moniker: impl Into<String>,
            target_url: impl Into<String>,
        ) -> Arc<dyn LogReader> {
            Arc::new(Self {
                log_manager,
                target_moniker: target_moniker.into(),
                target_url: target_url.into(),
            })
        }
    }

    impl LogReader for EventStreamLogReader {
        fn handle_request(
            &self,
            log_sender: mpsc::UnboundedSender<FutureObj<'static, ()>>,
        ) -> LogSinkProxy {
            let (event_stream_proxy, event_stream) =
                fidl::endpoints::create_proxy_and_stream::<fsys::EventStreamMarker>().unwrap();
            let (log_sink_proxy, log_sink_server_end) =
                fidl::endpoints::create_proxy::<LogSinkMarker>().unwrap();
            event_stream_proxy
                .on_event(create_capability_requested_event(
                    self.target_moniker.clone(),
                    self.target_url.clone(),
                    log_sink_server_end.into_channel(),
                ))
                .unwrap();
            let log_manager = self.log_manager.clone();
            fasync::spawn(log_manager.handle_event_stream(event_stream, log_sender));

            log_sink_proxy
        }
    }

    struct TestStream<E> {
        sin: zx::Socket,
        _log_sink_proxy: LogSinkProxy,
        _encoder: PhantomData<E>,
    }

    impl<E, P> TestStream<E>
    where
        E: LogWriter<Packet = P>,
    {
        fn new(log_reader: Arc<dyn LogReader>) -> Self {
            let (sin, sout) = zx::Socket::create(zx::SocketOpts::DATAGRAM).unwrap();

            let (log_sender, log_receiver) = mpsc::unbounded();
            let log_sink_proxy = log_reader.handle_request(log_sender);

            fasync::spawn(log_receiver.for_each_concurrent(None, |rx| async move { rx.await }));

            E::connect(&log_sink_proxy, sout);

            Self { _log_sink_proxy: log_sink_proxy, sin, _encoder: PhantomData }
        }

        fn write_packets(&mut self, packets: Vec<P>) {
            for p in packets {
                self.write_packet(&p);
            }
        }

        fn write_packet(&mut self, packet: &P) {
            E::write(&self.sin, packet);
        }
    }

    /// Run a test on logs from klog, returning the inspector object.
    async fn debuglog_test(
        expected: impl IntoIterator<Item = LogMessage>,
        debug_log: TestDebugLog,
    ) -> inspect::Inspector {
        let inspector = inspect::Inspector::new();
        let inner = Arc::new(Mutex::new(ManagerInner {
            listeners: Pool::default(),
            interest_dispatcher: InterestDispatcher::default(),
            log_msg_buffer: buffer::MemoryBoundedBuffer::new(OLD_MSGS_BUF_SIZE),
            stats: stats::LogManagerStats::new_detached(),
            inspect_node: inspect::Node::default(),
        }));

        let lm = LogManager { inner }.with_inspect(inspector.root(), "log_stats").unwrap();
        let (log_proxy, log_stream) =
            fidl::endpoints::create_proxy_and_stream::<LogMarker>().unwrap();
        fasync::spawn(lm.clone().handle_log(log_stream));
        fasync::spawn(lm.drain_debuglog(debug_log));

        validate_log_stream(expected, log_proxy, None).await;
        inspector
    }

    fn setup_default_packet() -> fx_log_packet_t {
        let mut p: fx_log_packet_t = Default::default();
        p.metadata.pid = 1;
        p.metadata.tid = 1;
        p.metadata.severity = LogLevelFilter::Warn.into_primitive().into();
        p.metadata.dropped_logs = 2;
        p.data[0] = 5;
        memset(&mut p.data[..], 1, 65, 5);
        memset(&mut p.data[..], 7, 66, 5);
        return p;
    }

    fn copy_log_message(log_message: &LogMessage) -> LogMessage {
        LogMessage {
            pid: log_message.pid,
            tid: log_message.tid,
            time: log_message.time,
            severity: log_message.severity,
            dropped_logs: log_message.dropped_logs,
            tags: log_message.tags.clone(),
            msg: log_message.msg.clone(),
        }
    }

    /// Function to convert fx_log_packet_t to &[u8].
    /// This function is safe as it works on `fx_log_packet_t` which
    /// doesn't have any uninitialized padding bits.
    fn to_u8_slice(p: &fx_log_packet_t) -> &[u8] {
        // This code just converts to &[u8] so no need to explicity drop it as memory
        // location would be freed as soon as p is dropped.
        unsafe {
            ::std::slice::from_raw_parts(
                (p as *const fx_log_packet_t) as *const u8,
                ::std::mem::size_of::<fx_log_packet_t>(),
            )
        }
    }

    fn memset<T: Copy>(x: &mut [T], offset: usize, value: T, size: usize) {
        x[offset..(offset + size)].iter_mut().for_each(|x| *x = value);
    }
}
