// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{data_repository::DiagnosticsDataRepository, diagnostics, inspect},
    anyhow::{format_err, Error},
    fidl::endpoints::ServerEnd,
    fidl_fuchsia_diagnostics::{
        ArchiveAccessorRequest, ArchiveAccessorRequestStream, BatchIteratorMarker,
        ClientSelectorConfiguration, DataType, Selector, SelectorArgument,
    },
    fuchsia_async as fasync,
    fuchsia_inspect::NumericProperty,
    fuchsia_zircon_status as zx_status,
    futures::{TryFutureExt, TryStreamExt},
    log::{error, warn},
    parking_lot::RwLock,
    selectors,
    std::sync::Arc,
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
) -> Result<Vec<Selector>, Error> {
    if selector_args.is_empty() {
        return Err(format_err!("Empty selectors are a misconfiguration."));
    }
    selector_args
        .into_iter()
        .map(|selector_arg| match selector_arg {
            SelectorArgument::StructuredSelector(x) => match selectors::validate_selector(&x) {
                Ok(_) => Ok(x),
                Err(e) => Err(format_err!("Error validating selector for inspect reading: {}", e)),
            },
            SelectorArgument::RawSelector(x) => selectors::parse_selector(&x).map_err(|e| {
                format_err!("Error parsing selector string for inspect reading: {}", e)
            }),
            _ => Err(format_err!("Unrecognized SelectorArgument type")),
        })
        .collect()
}

impl ArchiveAccessor {
    /// Create a new accessor for interacting with the archivist's data. The inspect_repo
    /// parameter determines which static configurations scope/restrict the visibility of inspect
    /// data accessed by readers spawned by this accessor.
    pub fn new(
        diagnostics_repo: Arc<RwLock<DiagnosticsDataRepository>>,
        archive_accessor_stats: Arc<diagnostics::ArchiveAccessorStats>,
    ) -> Self {
        ArchiveAccessor { diagnostics_repo, archive_accessor_stats }
    }

    fn handle_stream_inspect(
        &self,
        result_stream: ServerEnd<BatchIteratorMarker>,
        stream_parameters: fidl_fuchsia_diagnostics::StreamParameters,
    ) -> Result<(), Error> {
        let inspect_reader_server_stats = Arc::new(diagnostics::InspectReaderServerStats::new(
            self.archive_accessor_stats.clone(),
        ));

        match (
            stream_parameters.stream_mode,
            stream_parameters.format,
            stream_parameters.client_selector_configuration,
        ) {
            (None, _, _) | (_, None, _) | (_, _, None) => {
                warn!("Client was missing required stream parameters.");

                result_stream.close_with_epitaph(zx_status::Status::INVALID_ARGS).unwrap_or_else(
                    |e| error!("Unable to write epitaph to result stream: {:?}", e),
                );
                Ok(())
            }
            (Some(stream_mode), Some(format), Some(selector_config)) => match selector_config {
                ClientSelectorConfiguration::Selectors(selectors) => {
                    match validate_and_parse_inspect_selectors(selectors) {
                        Ok(selectors) => {
                            let inspect_reader_server = inspect::ReaderServer::new(
                                self.diagnostics_repo.clone(),
                                Some(selectors),
                                inspect_reader_server_stats,
                            );

                            inspect_reader_server
                                .stream_inspect(stream_mode, format, result_stream)
                                .unwrap_or_else(|_| {
                                    warn!("Inspect Reader session crashed.");
                                });
                            Ok(())
                        }
                        Err(e) => {
                            warn!(
                                "Client provided invalid selectors to diagnostics stream: {:?}",
                                e
                            );

                            result_stream
                                .close_with_epitaph(zx_status::Status::WRONG_TYPE)
                                .unwrap_or_else(|e| {
                                    warn!("Unable to write epitaph to result stream: {:?}", e)
                                });

                            Ok(())
                        }
                    }
                }
                ClientSelectorConfiguration::SelectAll(_) => {
                    let inspect_reader_server = inspect::ReaderServer::new(
                        self.diagnostics_repo.clone(),
                        None,
                        inspect_reader_server_stats,
                    );

                    inspect_reader_server
                        .stream_inspect(stream_mode, format, result_stream)
                        .unwrap_or_else(|_| {
                            warn!("Inspect Reader session crashed.");
                        });
                    Ok(())
                }
                _ => {
                    warn!("Unable to process requested selector configuration.");

                    result_stream
                        .close_with_epitaph(zx_status::Status::INVALID_ARGS)
                        .unwrap_or_else(|e| {
                            warn!("Unable to write epitaph to result stream: {:?}", e)
                        });

                    Ok(())
                }
            },
        }
    }

    /// Spawn an instance `fidl_fuchsia_diagnostics/Archive` that allows clients to open
    /// reader session to diagnostics data.
    pub fn spawn_archive_accessor_server(self, mut stream: ArchiveAccessorRequestStream) {
        // Self isn't guaranteed to live into the exception handling of the async block. We need to clone self
        // to have a version that can be referenced in the exception handling.
        let errorful_archive_accessor_stats = self.archive_accessor_stats.clone();
        fasync::spawn(
            async move {
                self.archive_accessor_stats.global_archive_accessor_connections_opened.add(1);
                while let Some(req) = stream.try_next().await? {
                    match req {
                        ArchiveAccessorRequest::StreamDiagnostics {
                            result_stream,
                            stream_parameters,
                            control_handle: _,
                        } => {
                            self.archive_accessor_stats.global_stream_diagnostics_requests.add(1);
                            match stream_parameters.data_type {
                                Some(DataType::Inspect) => {
                                    self.handle_stream_inspect(result_stream, stream_parameters)?
                                }
                                None => {
                                    warn!("Client failed to specify a valid data type.");

                                    result_stream
                                        .close_with_epitaph(zx_status::Status::INVALID_ARGS)
                                        .unwrap_or_else(|e| {
                                            warn!(
                                                "Unable to write epitaph to result stream: {:?}",
                                                e
                                            )
                                        });
                                }
                            }
                        }
                    }
                }
                self.archive_accessor_stats.global_archive_accessor_connections_closed.add(1);
                Ok(())
            }
            .unwrap_or_else(move |e: anyhow::Error| {
                errorful_archive_accessor_stats.global_archive_accessor_connections_closed.add(1);
                error!("couldn't run archive accessor service: {:?}", e)
            }),
        );
    }
}
