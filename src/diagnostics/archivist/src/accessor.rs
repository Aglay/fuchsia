// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        constants::FORMATTED_CONTENT_CHUNK_SIZE_TARGET,
        diagnostics::{self, ArchiveAccessorStats, DiagnosticsServerStats},
        error::AccessorError,
        formatter::{new_batcher, FormattedStream, JsonPacketSerializer, JsonString},
        inspect,
        lifecycle::LifecycleServer,
        repository::DiagnosticsDataRepository,
    },
    anyhow::format_err,
    diagnostics_data::{Data, DiagnosticsData},
    fidl_fuchsia_diagnostics::{
        self, ArchiveAccessorRequest, ArchiveAccessorRequestStream, BatchIteratorRequest,
        BatchIteratorRequestStream, ClientSelectorConfiguration, DataType, Format, Selector,
        SelectorArgument, StreamMode,
    },
    fuchsia_async::{self as fasync, Task},
    fuchsia_inspect::NumericProperty,
    fuchsia_zircon as zx,
    futures::prelude::*,
    parking_lot::RwLock,
    selectors,
    serde::Serialize,
    std::sync::Arc,
    tracing::warn,
};

/// ArchiveAccessor represents an incoming connection from a client to an Archivist
/// instance, through which the client may make Reader requests to the various data
/// sources the Archivist offers.
pub struct ArchiveAccessor {
    // The inspect repository containing read-only inspect data shared across
    // all inspect reader instances.
    diagnostics_repo: Arc<RwLock<DiagnosticsDataRepository>>,
    archive_accessor_stats: Arc<diagnostics::ArchiveAccessorStats>,
}

fn validate_and_parse_inspect_selectors(
    selector_args: Vec<SelectorArgument>,
) -> Result<Vec<Selector>, AccessorError> {
    let mut selectors = vec![];
    if selector_args.is_empty() {
        Err(AccessorError::EmptySelectors)?;
    }

    for selector_arg in selector_args {
        let selector = match selector_arg {
            SelectorArgument::StructuredSelector(s) => selectors::validate_selector(&s).map(|_| s),
            SelectorArgument::RawSelector(r) => selectors::parse_selector(&r),
            _ => Err(format_err!("unrecognized selector configuration")),
        }
        .map_err(AccessorError::ParseSelectors)?;

        selectors.push(selector);
    }

    Ok(selectors)
}

impl ArchiveAccessor {
    /// Create a new accessor for interacting with the archivist's data. The inspect_repo
    /// parameter determines which static configurations scope/restrict the visibility of inspect
    /// data accessed by readers spawned by this accessor.
    pub fn new(
        diagnostics_repo: Arc<RwLock<DiagnosticsDataRepository>>,
        archive_accessor_stats: Arc<ArchiveAccessorStats>,
    ) -> Self {
        ArchiveAccessor { diagnostics_repo, archive_accessor_stats }
    }

    async fn run_server(
        diagnostics_repo: Arc<RwLock<DiagnosticsDataRepository>>,
        requests: BatchIteratorRequestStream,
        params: fidl_fuchsia_diagnostics::StreamParameters,
        accessor_stats: Arc<ArchiveAccessorStats>,
    ) -> Result<(), AccessorError> {
        let format = params.format.ok_or(AccessorError::MissingFormat)?;
        if !matches!(format, Format::Json) {
            return Err(AccessorError::UnsupportedFormat);
        }
        let mode = params.stream_mode.ok_or(AccessorError::MissingMode)?;

        match params.data_type.ok_or(AccessorError::MissingDataType)? {
            DataType::Inspect => {
                if !matches!(mode, StreamMode::Snapshot) {
                    return Err(AccessorError::UnsupportedMode);
                }
                let stats = Arc::new(DiagnosticsServerStats::for_inspect(accessor_stats));

                let selectors =
                    params.client_selector_configuration.ok_or(AccessorError::MissingSelectors)?;
                let selectors = match selectors {
                    ClientSelectorConfiguration::Selectors(selectors) => {
                        Some(validate_and_parse_inspect_selectors(selectors)?)
                    }
                    ClientSelectorConfiguration::SelectAll(_) => None,
                    _ => Err(AccessorError::InvalidSelectors("unrecognized selectors"))?,
                };

                BatchIterator::new(
                    inspect::ReaderServer::stream(
                        diagnostics_repo,
                        params.batch_retrieval_timeout_seconds,
                        selectors,
                        stats.clone(),
                    ),
                    requests,
                    mode,
                    stats,
                )?
                .run()
                .await
            }
            DataType::Lifecycle => {
                // TODO(fxbug.dev/61350) support other modes
                if !matches!(mode, StreamMode::Snapshot) {
                    return Err(AccessorError::UnsupportedMode);
                }
                let stats = Arc::new(DiagnosticsServerStats::for_lifecycle(accessor_stats));

                let selectors =
                    params.client_selector_configuration.ok_or(AccessorError::MissingSelectors)?;
                if !matches!(selectors, ClientSelectorConfiguration::SelectAll(_)) {
                    Err(AccessorError::InvalidSelectors(
                        "lifecycle only supports SelectAll at the moment",
                    ))?;
                }

                let events = LifecycleServer::new(diagnostics_repo);

                BatchIterator::new(events, requests, mode, stats)?.run().await
            }
            DataType::Logs => {
                let stats = Arc::new(DiagnosticsServerStats::for_logs(accessor_stats));
                let (manager, redactor) = {
                    let repo = diagnostics_repo.read();
                    (repo.log_manager(), repo.log_redactor())
                };
                let logs = redactor.redact_stream(manager.cursor(mode).await);
                BatchIterator::new_serving_arrays(logs, requests, mode, stats)?.run().await
            }
        }
    }

    /// Spawn an instance `fidl_fuchsia_diagnostics/Archive` that allows clients to open
    /// reader session to diagnostics data.
    pub fn spawn_archive_accessor_server(self, mut stream: ArchiveAccessorRequestStream) {
        // Self isn't guaranteed to live into the exception handling of the async block. We need to clone self
        // to have a version that can be referenced in the exception handling.
        fasync::Task::spawn(async move {
            self.archive_accessor_stats.global_stats.archive_accessor_connections_opened.add(1);
            while let Ok(Some(ArchiveAccessorRequest::StreamDiagnostics {
                result_stream,
                stream_parameters,
                control_handle: _,
            })) = stream.try_next().await
            {
                let (requests, control) = match result_stream.into_stream_and_control_handle() {
                    Ok(r) => r,
                    Err(e) => {
                        warn!(?e, "Couldn't bind results channel to executor.");
                        continue;
                    }
                };

                self.archive_accessor_stats.global_stats.stream_diagnostics_requests.add(1);
                let repo = self.diagnostics_repo.clone();
                let accessor_stats = self.archive_accessor_stats.clone();
                Task::spawn(async move {
                    if let Err(e) =
                        Self::run_server(repo, requests, stream_parameters, accessor_stats).await
                    {
                        e.close(control);
                    }
                })
                .detach()
            }
            self.archive_accessor_stats.global_stats.archive_accessor_connections_closed.add(1);
        })
        .detach();
    }
}

pub struct BatchIterator {
    requests: BatchIteratorRequestStream,
    stats: Arc<DiagnosticsServerStats>,
    data: FormattedStream,
}

impl BatchIterator {
    pub fn new<Items, D>(
        data: Items,
        requests: BatchIteratorRequestStream,
        mode: StreamMode,
        stats: Arc<DiagnosticsServerStats>,
    ) -> Result<Self, AccessorError>
    where
        Items: Stream<Item = Data<D>> + Send + 'static,
        D: DiagnosticsData,
    {
        let result_stats = stats.clone();
        let data = data.map(move |d| {
            if D::has_errors(&d.metadata) {
                result_stats.add_result_error();
            }
            let res = JsonString::serialize(&d);
            if res.is_err() {
                result_stats.add_result_error();
            }
            result_stats.add_result();
            res
        });

        Self::new_inner(new_batcher(data, stats.clone(), mode), requests, stats)
    }

    pub fn new_serving_arrays<D, S>(
        data: S,
        requests: BatchIteratorRequestStream,
        mode: StreamMode,
        stats: Arc<DiagnosticsServerStats>,
    ) -> Result<Self, AccessorError>
    where
        D: Serialize,
        S: Stream<Item = D> + Send + Unpin + 'static,
    {
        let data =
            JsonPacketSerializer::new(stats.clone(), FORMATTED_CONTENT_CHUNK_SIZE_TARGET, data);
        Self::new_inner(new_batcher(data, stats.clone(), mode), requests, stats)
    }

    fn new_inner(
        data: FormattedStream,
        requests: BatchIteratorRequestStream,
        stats: Arc<DiagnosticsServerStats>,
    ) -> Result<Self, AccessorError> {
        stats.open_connection();
        Ok(Self { data, requests, stats })
    }

    pub async fn run(mut self) -> Result<(), AccessorError> {
        while let Some(res) = self.requests.next().await {
            let BatchIteratorRequest::GetNext { responder } = res?;
            self.stats.add_request();
            let start_time = zx::Time::get_monotonic();
            // if we get None back, treat that as a terminal batch with an empty vec
            let batch = self.data.next().await.unwrap_or(vec![]);
            // turn errors into epitaphs -- we drop intermediate items if there was an error midway
            let batch = batch.into_iter().collect::<Result<Vec<_>, _>>()?;

            // increment counters
            self.stats.add_response();
            if batch.is_empty() {
                self.stats.add_terminal();
            }
            self.stats.global_stats().record_batch_duration(zx::Time::get_monotonic() - start_time);

            let mut response = Ok(batch);
            responder.send(&mut response)?;
        }
        Ok(())
    }
}

impl Drop for BatchIterator {
    fn drop(&mut self) {
        self.stats.close_connection();
    }
}
