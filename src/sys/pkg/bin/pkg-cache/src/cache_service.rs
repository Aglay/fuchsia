// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::blobs::{open_blob, BlobKind, OpenBlob, OpenBlobError, OpenBlobSuccess},
    anyhow::{anyhow, Context as _, Error},
    cobalt_sw_delivery_registry as metrics,
    fidl::endpoints::ServerEnd,
    fidl_fuchsia_io::{DirectoryMarker, FileRequest, FileRequestStream},
    fidl_fuchsia_pkg::{
        BlobInfoIteratorNextResponder, BlobInfoIteratorRequest, BlobInfoIteratorRequestStream,
        NeededBlobsMarker, NeededBlobsRequest, NeededBlobsRequestStream, PackageCacheRequest,
        PackageCacheRequestStream, PackageIndexEntry, PackageIndexIteratorNextResponder,
        PackageIndexIteratorRequest, PackageIndexIteratorRequestStream, PackageUrl,
    },
    fidl_fuchsia_pkg_ext::{BlobId, BlobInfo, Measurable},
    fuchsia_async::Task,
    fuchsia_cobalt::CobaltSender,
    fuchsia_hash::Hash,
    fuchsia_syslog::{fx_log_err, fx_log_info, fx_log_warn},
    fuchsia_trace as trace,
    fuchsia_zircon::{sys::ZX_CHANNEL_MAX_MSG_BYTES, Status},
    futures::{prelude::*, stream::FuturesUnordered},
    std::{collections::HashSet, sync::Arc},
    system_image::StaticPackages,
};

// FIXME(52297) This constant would ideally be exported by the `fidl` crate.
// sizeof(TransactionHeader) + sizeof(VectorHeader)
const FIDL_VEC_RESPONSE_OVERHEAD_BYTES: usize = 32;

pub async fn serve(
    pkgfs_versions: pkgfs::versions::Client,
    pkgfs_ctl: pkgfs::control::Client,
    static_packages: Arc<StaticPackages>,
    stream: PackageCacheRequestStream,
    cobalt_sender: CobaltSender,
) -> Result<(), Error> {
    stream
        .map_err(anyhow::Error::new)
        .try_for_each_concurrent(None, |event| async {
            let cobalt_sender = cobalt_sender.clone();
            match event {
                PackageCacheRequest::Get {
                    meta_far_blob,
                    selectors,
                    needed_blobs,
                    dir,
                    responder,
                } => {
                    let meta_far_blob: BlobInfo = meta_far_blob.into();
                    trace::duration_begin!("app", "cache_get",
                        "meta_far_blob_id" => meta_far_blob.blob_id.to_string().as_str()
                    );
                    let response = get(
                        &pkgfs_versions,
                        meta_far_blob,
                        selectors,
                        needed_blobs,
                        dir,
                        cobalt_sender,
                    )
                    .await;
                    trace::duration_end!("app", "cache_get",
                        "status" => Status::from(response).to_string().as_str()
                    );
                    responder.send(&mut response.map_err(|status| status.into_raw()))?;
                }
                PackageCacheRequest::Open { meta_far_blob_id, selectors, dir, responder } => {
                    let meta_far_blob_id: BlobId = meta_far_blob_id.into();
                    trace::duration_begin!("app", "cache_open",
                        "meta_far_blob_id" => meta_far_blob_id.to_string().as_str()
                    );
                    let response =
                        open(&pkgfs_versions, meta_far_blob_id, selectors, dir, cobalt_sender)
                            .await;
                    trace::duration_end!("app", "cache_open",
                        "status" => Status::from(response).to_string().as_str()
                    );
                    responder.send(&mut response.map_err(|status| status.into_raw()))?;
                }
                PackageCacheRequest::BasePackageIndex { iterator, control_handle: _ } => {
                    let stream = iterator.into_stream()?;
                    serve_base_package_index(Arc::clone(&static_packages), stream).await;
                }
                PackageCacheRequest::Sync { responder } => {
                    responder.send(&mut pkgfs_ctl.sync().await.map_err(|e| {
                        fx_log_err!("error syncing /pkgfs/ctl: {:#}", anyhow!(e));
                        Status::INTERNAL.into_raw()
                    }))?;
                }
            }

            Ok(())
        })
        .await
}

/// Fetch a package, and optionally mount it.
///
/// TODO: implement this method. This stub can't simply proxy to amber for now, since it doesn't
/// know the name of the package, and amber needs it to lookup the package in its TUF repo.
async fn get<'a>(
    pkgfs_versions: &'a pkgfs::versions::Client,
    meta_far_blob: BlobInfo,
    selectors: Vec<String>,
    _needed_blobs: ServerEnd<NeededBlobsMarker>,
    dir_request: Option<ServerEnd<DirectoryMarker>>,
    cobalt_sender: CobaltSender,
) -> Result<(), Status> {
    fx_log_info!("fetching {:?} with the selectors {:?}", meta_far_blob, selectors);

    if let Some(dir_request) = dir_request {
        open(pkgfs_versions, meta_far_blob.blob_id, selectors.clone(), dir_request, cobalt_sender)
            .await?;

        Ok(())
    } else {
        Err(Status::NOT_SUPPORTED)
    }
}

/// Open a package directory.
async fn open<'a>(
    pkgfs_versions: &'a pkgfs::versions::Client,
    meta_far_blob_id: BlobId,
    selectors: Vec<String>,
    dir_request: ServerEnd<DirectoryMarker>,
    mut cobalt_sender: CobaltSender,
) -> Result<(), Status> {
    // FIXME: need to implement selectors.
    if !selectors.is_empty() {
        fx_log_warn!("resolve does not support selectors yet");
    }

    let pkg =
        pkgfs_versions.open_package(&meta_far_blob_id.into()).await.map_err(|err| match err {
            pkgfs::package::OpenError::NotFound => {
                cobalt_sender.log_event_count(
                    metrics::PKG_CACHE_OPEN_METRIC_ID,
                    metrics::PkgCacheOpenMetricDimensionResult::NotFound,
                    0,
                    1,
                );
                Status::NOT_FOUND
            }
            err => {
                cobalt_sender.log_event_count(
                    metrics::PKG_CACHE_OPEN_METRIC_ID,
                    metrics::PkgCacheOpenMetricDimensionResult::Io,
                    0,
                    1,
                );
                fx_log_err!("error opening {}: {:?}", meta_far_blob_id, err);
                Status::INTERNAL
            }
        })?;

    pkg.reopen(dir_request).map_err(|err| {
        fx_log_err!("error opening {}: {:#}", meta_far_blob_id, anyhow!(err));
        cobalt_sender.log_event_count(
            metrics::PKG_CACHE_OPEN_METRIC_ID,
            metrics::PkgCacheOpenMetricDimensionResult::Io,
            0,
            1,
        );
        Status::INTERNAL
    })?;

    cobalt_sender.log_event_count(
        metrics::PKG_CACHE_OPEN_METRIC_ID,
        metrics::PkgCacheOpenMetricDimensionResult::Success,
        0,
        1,
    );
    Ok(())
}

#[derive(thiserror::Error, Debug)]
enum ServeNeededBlobsError {
    #[error("protocol violation: request stream terminated unexpectedly")]
    UnexpectedClose,

    #[error("protocol violation: expected {expected} request, got {received}")]
    UnexpectedRequest { received: &'static str, expected: &'static str },

    #[error("protocol violation: while reading next request")]
    ReceiveRequest(#[source] fidl::Error),

    #[error("protocol violation: while responding to last request")]
    SendResponse(#[source] fidl::Error),

    #[error("while opening {context} for write")]
    OpenBlob {
        context: BlobContext,
        #[source]
        source: OpenBlobError,
    },

    #[error("while writing {context}")]
    WriteBlob {
        context: BlobContext,
        #[source]
        source: ServeWriteBlobError,
    },

    #[error("while listing needs")]
    ListNeeds(#[source] pkgfs::needs::ListNeedsError),
}

#[derive(Debug)]
struct BlobContext {
    kind: BlobKind,
    hash: Hash,
}

impl BlobContext {
    fn kind_name(&self) -> &'static str {
        match self.kind {
            BlobKind::Package => "metadata",
            BlobKind::Data => "data",
        }
    }
}

impl std::fmt::Display for BlobContext {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{} blob ({})", self.kind_name(), self.hash)
    }
}

/// Implements the fuchsia.pkg.NeededBlobs protocol, which represents the transaction for caching a
/// particular package.
///
/// Clients should start by requesting to `OpenMetaBlob()`, and fetch and write the metadata blob
/// if needed. Once written, `GetMissingBlobs()` should be used to determine which content blobs
/// need fetched and written using `OpenBlob()`. Violating the expected protocol state will result
/// in the channel being closed by the package cache with a `ZX_ERR_BAD_STATE` epitaph and aborting
/// the package cache operation.
///
/// Once all needed blobs are written by the client, the package cache will complete the pending
/// [`PackageCache.Get`] request and close this channel with a `ZX_OK` epitaph.
#[cfg_attr(not(test), allow(dead_code))]
async fn serve_needed_blobs(
    mut stream: NeededBlobsRequestStream,
    meta_far_info: BlobInfo,
    pkgfs_install: &pkgfs::install::Client,
    pkgfs_needs: &pkgfs::needs::Client,
) -> Result<(), ServeNeededBlobsError> {
    let tasks = FuturesUnordered::new();

    enum State {
        ExpectOpenMetaBlob,
        ExpectGetMissingBlobs,
        ExpectOpenContentBlob { needs: HashSet<Hash> },
    }

    impl State {
        fn expectation(&self) -> &'static str {
            match self {
                State::ExpectOpenMetaBlob => "open_meta_blob",
                State::ExpectGetMissingBlobs => "get_missing_blobs",
                State::ExpectOpenContentBlob { .. } => "open_blob",
            }
        }
    }

    let mut state = State::ExpectOpenMetaBlob;

    loop {
        let request = stream.try_next().await.map_err(ServeNeededBlobsError::ReceiveRequest)?;

        state = match (state, request) {
            // Step 1: Open and write the meta.far, or determine it is not needed.
            (
                state @ State::ExpectOpenMetaBlob,
                Some(NeededBlobsRequest::OpenMetaBlob { file, responder }),
            ) => {
                let file_stream =
                    file.into_stream().map_err(ServeNeededBlobsError::ReceiveRequest)?;

                let open_res =
                    open_blob(pkgfs_install, meta_far_info.blob_id.into(), BlobKind::Package).await;

                // Always respond to the OpenMetaBlob request, then worry about actually handling
                // the result.
                responder
                    .send(&mut match &open_res {
                        Ok(OpenBlobSuccess::Needed(_)) => Ok(true),
                        Ok(OpenBlobSuccess::AlreadyExists) => Ok(false),
                        Err(OpenBlobError::ConcurrentWrite) => {
                            Err(fidl_fuchsia_pkg::OpenBlobError::ConcurrentWrite)
                        }
                        Err(OpenBlobError::Io(_)) => {
                            Err(fidl_fuchsia_pkg::OpenBlobError::UnspecifiedIo)
                        }
                    })
                    .map_err(ServeNeededBlobsError::SendResponse)?;

                match open_res {
                    Ok(OpenBlobSuccess::Needed(blob)) => {
                        match serve_write_blob(file_stream, blob).await {
                            Ok(()) => State::ExpectGetMissingBlobs,
                            Err(e) if e.is_fatal() => {
                                return Err(ServeNeededBlobsError::WriteBlob {
                                    context: BlobContext {
                                        kind: BlobKind::Package,
                                        hash: meta_far_info.blob_id.into(),
                                    },
                                    source: e,
                                });
                            }
                            Err(e) => {
                                fx_log_warn!(
                                    "Non-fatal error while writing metadata blob: {:#}",
                                    anyhow!(e)
                                );
                                state
                            }
                        }
                    }
                    Ok(OpenBlobSuccess::AlreadyExists) => State::ExpectGetMissingBlobs,
                    Err(OpenBlobError::ConcurrentWrite) => {
                        fx_log_warn!(
                            "Non-fatal error while opening metadata blob: {:#}",
                            anyhow!(OpenBlobError::ConcurrentWrite)
                        );
                        state
                    }
                    Err(e @ OpenBlobError::Io(_)) => {
                        return Err(ServeNeededBlobsError::OpenBlob {
                            context: BlobContext {
                                kind: BlobKind::Package,
                                hash: meta_far_info.blob_id.into(),
                            },
                            source: e,
                        });
                    }
                }
            }

            // Step 2: Determine which data blobs are needed and report them to the client.
            (
                State::ExpectGetMissingBlobs,
                Some(NeededBlobsRequest::GetMissingBlobs { iterator, control_handle: _ }),
            ) => {
                let iter_stream =
                    iterator.into_stream().map_err(ServeNeededBlobsError::ReceiveRequest)?;

                // list_needs produces a stream that produces the full set of currently missing
                // blobs on-demand as items are read from the stream.  We are only interested in
                // querying the needs once, so we only need to read 1 item and can then drop the
                // stream.
                let needs = pkgfs_needs.list_needs(meta_far_info.blob_id.into());
                futures::pin_mut!(needs);
                let needs =
                    match needs.try_next().await.map_err(ServeNeededBlobsError::ListNeeds)? {
                        Some(needs) => {
                            let mut needs = needs
                                .into_iter()
                                .map(|hash| BlobInfo { blob_id: hash.into(), length: 0 })
                                .collect::<Vec<_>>();
                            // The needs provided by the stream are stored in a HashSet, so needs
                            // are in an unspecified order here. Provide a deterministic ordering
                            // to test/callers by sorting on merkle root.
                            needs.sort_unstable();
                            needs
                        }
                        None => vec![],
                    };

                // Start serving the iterator in the background and internally move on to the next
                // state. If this foreground task decides to bail out, tasks will be dropped which
                // will cancel any incomplete background tasks.
                let serve_iterator = Task::spawn(serve_blob_info_iterator(
                    needs
                        .iter()
                        .cloned()
                        .map(Into::into)
                        .collect::<Vec<fidl_fuchsia_pkg::BlobInfo>>(),
                    iter_stream,
                ));
                tasks.push(serve_iterator);

                State::ExpectOpenContentBlob {
                    needs: needs.into_iter().map(|need| need.blob_id.into()).collect(),
                }
            }

            // Step 3: Open and write all needed data blobs.
            (
                State::ExpectOpenContentBlob { needs },
                Some(NeededBlobsRequest::OpenBlob { blob_id, file, responder }),
            ) => {
                // TODO(64622) implement
                let _ = (needs, blob_id, file, responder);
                todo!();
            }

            (state, Some(request)) => {
                return Err(ServeNeededBlobsError::UnexpectedRequest {
                    received: request.method_name(),
                    expected: state.expectation(),
                });
            }

            (_, None) => {
                return Err(ServeNeededBlobsError::UnexpectedClose);
            }
        };
    }
}

#[derive(thiserror::Error, Debug)]
enum ServeWriteBlobError {
    #[error("protocol violation: file request stream terminated unexpectedly")]
    UnexpectedClose,

    #[error("protocol violation: file request stream fidl error")]
    Fidl(#[source] fidl::Error),

    #[error("protocol violation: expected {expected} request, got {received}")]
    UnexpectedRequest { received: &'static str, expected: &'static str },

    #[error("insufficient storage space is available")]
    NoSpace,

    #[error("the provided blob data is corrupt")]
    Corrupt,

    #[error("while truncating the blob")]
    Truncate(#[source] pkgfs::install::BlobTruncateError),

    #[error("while writing to the blob")]
    Write(#[source] pkgfs::install::BlobWriteError),
}

impl From<pkgfs::install::BlobTruncateError> for ServeWriteBlobError {
    fn from(e: pkgfs::install::BlobTruncateError) -> Self {
        match e {
            pkgfs::install::BlobTruncateError::NoSpace => ServeWriteBlobError::NoSpace,
            e => ServeWriteBlobError::Truncate(e),
        }
    }
}

impl From<pkgfs::install::BlobWriteError> for ServeWriteBlobError {
    fn from(e: pkgfs::install::BlobWriteError) -> Self {
        match e {
            pkgfs::install::BlobWriteError::NoSpace => ServeWriteBlobError::NoSpace,
            pkgfs::install::BlobWriteError::Corrupt => ServeWriteBlobError::Corrupt,
            e => ServeWriteBlobError::Write(e),
        }
    }
}

impl ServeWriteBlobError {
    /// Determines if this error should cancel the associated Get() operation (true) or should
    /// allow the NeededBlobs client retry the operation later (false).
    fn is_fatal(&self) -> bool {
        match self {
            ServeWriteBlobError::UnexpectedClose => true,
            ServeWriteBlobError::Fidl(_) => true,
            ServeWriteBlobError::UnexpectedRequest { .. } => true,
            ServeWriteBlobError::NoSpace => false,
            ServeWriteBlobError::Corrupt => false,
            ServeWriteBlobError::Truncate(_) => true,
            ServeWriteBlobError::Write(_) => true,
        }
    }
}

async fn serve_write_blob(
    mut stream: FileRequestStream,
    blob: OpenBlob,
) -> Result<(), ServeWriteBlobError> {
    use pkgfs::install::{
        Blob, BlobTruncateError, BlobWriteError, BlobWriteSuccess, NeedsData, NeedsTruncate,
    };

    let OpenBlob { blob, closer } = blob;

    enum State {
        ExpectTruncate(Blob<NeedsTruncate>),
        ExpectData(Blob<NeedsData>),
        ExpectClose,
    }

    impl State {
        fn expectation(&self) -> &'static str {
            match self {
                State::ExpectTruncate(_) => "truncate",
                State::ExpectData(_) => "write",
                State::ExpectClose => "close",
            }
        }
    }

    // Allow the inner task to sometimes close the underlying blob early while also unconditionally
    // calling close after the inner task completes.  Close closes the underlying blob the first
    // time it is called and becomes a no-op on later calls.
    let mut closer = Some(closer);
    let mut close = || {
        let closer = closer.take().map(|closer| closer.close());
        async move {
            match closer {
                Some(closer) => closer.await,
                None => {}
            }
        }
    };

    let mut state = State::ExpectTruncate(blob);

    let task = async {
        while let Some(request) = stream.try_next().await.map_err(ServeWriteBlobError::Fidl)? {
            state = match (request, state) {
                (FileRequest::Truncate { length, responder }, State::ExpectTruncate(blob)) => {
                    let res = blob.truncate(length).await;

                    // Interpret responding errors as the stream closing unexpectedly.
                    let _ = responder.send(
                        match &res {
                            Ok(_) => Status::OK,
                            Err(BlobTruncateError::NoSpace) => Status::NO_SPACE,
                            Err(BlobTruncateError::Fidl(_))
                            | Err(BlobTruncateError::UnexpectedResponse(_)) => Status::INTERNAL,
                        }
                        .into_raw(),
                    );

                    let blob = res?;
                    State::ExpectData(blob)
                }

                (FileRequest::Write { data, responder }, State::ExpectData(blob)) => {
                    let res = blob.write(&data).await;

                    let _ = responder.send(
                        match &res {
                            Ok(_) => Status::OK,
                            Err(BlobWriteError::NoSpace) => Status::NO_SPACE,
                            Err(BlobWriteError::Corrupt) => Status::IO_DATA_INTEGRITY,
                            Err(BlobWriteError::Overwrite) => Status::IO,
                            Err(BlobWriteError::Fidl(_))
                            | Err(BlobWriteError::UnexpectedResponse(_)) => Status::INTERNAL,
                        }
                        .into_raw(),
                        data.len() as u64,
                    );

                    match res? {
                        BlobWriteSuccess::MoreToWrite(blob) => State::ExpectData(blob),
                        BlobWriteSuccess::Done => State::ExpectClose,
                    }
                }

                // Close is allowed in any state, but the blob is only written if we were expecting
                // a close.
                (FileRequest::Close { responder }, State::ExpectClose) => {
                    close().await;
                    let _ = responder.send(Status::OK.into_raw());
                    return Ok(());
                }
                (FileRequest::Close { responder }, _) => {
                    close().await;
                    let _ = responder.send(Status::OK.into_raw());
                    return Err(ServeWriteBlobError::UnexpectedClose);
                }

                (request, state) => {
                    return Err(ServeWriteBlobError::UnexpectedRequest {
                        received: request.method_name(),
                        expected: state.expectation(),
                    });
                }
            };
        }

        match state {
            State::ExpectClose => Ok(()),
            _ => Err(ServeWriteBlobError::UnexpectedClose),
        }
    };

    // Handle the request stream, then close the blob, then close the stream to avoid retry races
    // creating a blob that is still open.
    let res = task.await;
    close().await;
    drop(stream);

    res
}

/// Helper to split a slice of items into chunks that will fit in a single FIDL vec response.
///
/// Note, Chunker assumes the fixed overhead of a single fidl response header and a single vec
/// header per chunk.  It must not be used with more complex responses.
struct Chunker<'a, I> {
    items: &'a mut [I],
}

impl<'a, I> Chunker<'a, I>
where
    I: Measurable,
{
    fn new(items: &'a mut [I]) -> Self {
        Self { items }
    }

    /// Produce the next chunk of items to respond with. Iteration stops when this method returns
    /// an empty slice, which occurs when either:
    /// * All items have been returned
    /// * Chunker encounters an item so large that it cannot even be stored in a response
    ///   dedicated to just that one item.
    ///
    /// Once next() returns an empty slice, it will continue to do so in future calls.
    fn next(&mut self) -> &'a mut [I] {
        let mut bytes_used: usize = FIDL_VEC_RESPONSE_OVERHEAD_BYTES;
        let mut entry_count = 0;

        for entry in &*self.items {
            bytes_used += entry.measure();
            if bytes_used > ZX_CHANNEL_MAX_MSG_BYTES as usize {
                break;
            }
            entry_count += 1;
        }

        // tmp/swap dance to appease the borrow checker.
        let tmp = std::mem::replace(&mut self.items, &mut []);
        let (chunk, rest) = tmp.split_at_mut(entry_count);
        self.items = rest;
        chunk
    }
}

/// A FIDL request stream for a FIDL protocol following the iterator pattern.
trait FidlIteratorRequestStream:
    fidl::endpoints::RequestStream + TryStream<Error = fidl::Error>
{
    type Responder: FidlIteratorNextResponder;

    fn request_to_responder(request: <Self as TryStream>::Ok) -> Self::Responder;
}

/// A responder to a Next() request for a FIDL iterator.
trait FidlIteratorNextResponder {
    type Item: Measurable + fidl::encoding::Encodable;

    fn send_chunk(self, chunk: &mut [Self::Item]) -> Result<(), fidl::Error>;
}

impl FidlIteratorRequestStream for PackageIndexIteratorRequestStream {
    type Responder = PackageIndexIteratorNextResponder;

    fn request_to_responder(request: PackageIndexIteratorRequest) -> Self::Responder {
        let PackageIndexIteratorRequest::Next { responder } = request;
        responder
    }
}

impl FidlIteratorNextResponder for PackageIndexIteratorNextResponder {
    type Item = PackageIndexEntry;

    fn send_chunk(self, chunk: &mut [Self::Item]) -> Result<(), fidl::Error> {
        self.send(&mut chunk.iter_mut())
    }
}

impl FidlIteratorRequestStream for BlobInfoIteratorRequestStream {
    type Responder = BlobInfoIteratorNextResponder;

    fn request_to_responder(request: BlobInfoIteratorRequest) -> Self::Responder {
        let BlobInfoIteratorRequest::Next { responder } = request;
        responder
    }
}

impl FidlIteratorNextResponder for BlobInfoIteratorNextResponder {
    type Item = fidl_fuchsia_pkg::BlobInfo;

    fn send_chunk(self, chunk: &mut [Self::Item]) -> Result<(), fidl::Error> {
        self.send(&mut chunk.iter_mut())
    }
}

/// Serves the provided `FidlIteratorRequestStream` with as many entries per `Next()` request as
/// will fit in a fidl message. The task completes after yielding an empty response or the iterator
/// is interrupted (client closes the channel or this task encounters a FIDL layer error).
fn serve_fidl_iterator<I>(
    mut items: impl AsMut<[<I::Responder as FidlIteratorNextResponder>::Item]>,
    mut stream: I,
) -> impl Future<Output = ()>
where
    I: FidlIteratorRequestStream,
{
    async move {
        let mut items = Chunker::new(items.as_mut());

        loop {
            let mut chunk = items.next();

            let responder =
                match stream.try_next().await.context("while waiting for next() request")? {
                    None => break,
                    Some(request) => I::request_to_responder(request),
                };

            let () = responder.send_chunk(&mut chunk).context("while responding")?;

            // Yield a single empty chunk, then stop serving the protocol.
            if chunk.is_empty() {
                break;
            }
        }

        Ok(())
    }
    .unwrap_or_else(|e: anyhow::Error| {
        fx_log_err!(
            "error serving {} protocol: {:#}",
            <I::Service as fidl::endpoints::ServiceMarker>::DEBUG_NAME,
            anyhow!(e)
        )
    })
}

/// Serves the `PackageIndexIteratorRequestStream` with as many entries per request as will fit in
/// a fidl message.
async fn serve_base_package_index(
    static_packages: Arc<StaticPackages>,
    stream: PackageIndexIteratorRequestStream,
) {
    let package_entries = static_packages
        .contents()
        .map(|(path, hash)| PackageIndexEntry {
            package_url: PackageUrl { url: format!("fuchsia-pkg://fuchsia.com/{}", path.name()) },
            meta_far_blob_id: BlobId::from(hash.clone()).into(),
        })
        .collect::<Vec<PackageIndexEntry>>();

    serve_fidl_iterator(package_entries, stream).await
}

/// Serves the `BlobInfoIteratorRequestStream` with as many entries per request as will fit in a
/// fidl message.
#[cfg_attr(not(test), allow(dead_code))]
async fn serve_blob_info_iterator(
    items: impl AsMut<[fidl_fuchsia_pkg::BlobInfo]>,
    stream: BlobInfoIteratorRequestStream,
) {
    serve_fidl_iterator(items, stream).await
}

#[cfg(test)]
mod iter_tests {
    use {
        super::*,
        fidl_fuchsia_pkg::{BlobInfoIteratorMarker, PackageIndexIteratorMarker},
        fuchsia_async::Task,
        fuchsia_hash::HashRangeFull,
        fuchsia_pkg::PackagePath,
        proptest::prelude::*,
    };

    proptest! {
        #[test]
        fn blob_info_iterator_yields_expected_entries(items: Vec<BlobInfo>) {
            let mut executor = fuchsia_async::Executor::new().unwrap();
            executor.run_singlethreaded(async move {
                let (proxy, stream) =
                    fidl::endpoints::create_proxy_and_stream::<BlobInfoIteratorMarker>().unwrap();
                let mut actual_items = vec![];

                let ((), ()) = future::join(
                    async {
                        let items = items
                            .iter()
                            .cloned()
                            .map(fidl_fuchsia_pkg::BlobInfo::from)
                            .collect::<Vec<_>>();
                        serve_blob_info_iterator(items, stream).await
                    },
                    async {
                        loop {
                            let chunk = proxy.next().await.unwrap();
                            if chunk.is_empty() {
                                break;
                            }
                            let chunk = chunk.into_iter().map(BlobInfo::from);
                            actual_items.extend(chunk);
                        }
                    },
                )
                .await;

                assert_eq!(items, actual_items);
            })
        }
    }

    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    struct Byte(u8);

    impl Measurable for Byte {
        fn measure(&self) -> usize {
            1
        }
    }

    #[test]
    fn chunker_fuses() {
        let items = &mut [Byte(42)];
        let mut chunker = Chunker::new(items);

        assert_eq!(chunker.next(), &mut [Byte(42)]);
        assert_eq!(chunker.next(), &mut []);
        assert_eq!(chunker.next(), &mut []);
    }

    #[test]
    fn chunker_chunks_at_expected_boundary() {
        const BYTES_PER_CHUNK: usize =
            ZX_CHANNEL_MAX_MSG_BYTES as usize - FIDL_VEC_RESPONSE_OVERHEAD_BYTES;

        // Expect to fill 2 full chunks with 1 item left over.
        let mut items =
            (0..=(BYTES_PER_CHUNK as u64 * 2)).map(|n| Byte(n as u8)).collect::<Vec<Byte>>();
        let expected = items.clone();
        let mut chunker = Chunker::new(&mut items);

        let mut actual: Vec<Byte> = vec![];

        for _ in 0..2 {
            let chunk = chunker.next();
            assert_eq!(chunk.len(), BYTES_PER_CHUNK);

            actual.extend(&*chunk);
        }

        let chunk = chunker.next();
        assert_eq!(chunk.len(), 1);
        actual.extend(&*chunk);

        assert_eq!(actual, expected);
    }

    #[test]
    fn chunker_terminates_at_too_large_item() {
        #[derive(Debug, PartialEq, Eq)]
        struct TooBig;
        impl Measurable for TooBig {
            fn measure(&self) -> usize {
                ZX_CHANNEL_MAX_MSG_BYTES as usize
            }
        }

        let items = &mut [TooBig];
        let mut chunker = Chunker::new(items);
        assert_eq!(chunker.next(), &mut []);
    }

    const PACKAGE_INDEX_CHUNK_SIZE_MAX: usize = 818;
    const PACKAGE_INDEX_CHUNK_SIZE_MIN: usize = 372;

    #[test]
    fn verify_fidl_vec_response_overhead() {
        let vec_response_overhead = {
            use fidl::encoding::{TransactionHeader, TransactionMessage};

            let mut nop: Vec<()> = vec![];
            let mut msg =
                TransactionMessage { header: TransactionHeader::new(0, 0), body: &mut nop };

            fidl::encoding::with_tls_encoded(&mut msg, |bytes, _handles| {
                Result::<_, fidl::Error>::Ok(bytes.len())
            })
            .unwrap()
        };
        assert_eq!(vec_response_overhead, FIDL_VEC_RESPONSE_OVERHEAD_BYTES);
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn base_package_index_iterator_paginates_shortest_entries() {
        let names = ('a'..='z').cycle().map(|c| c.to_string());
        let paths = names.map(|name| PackagePath::from_name_and_variant(name, "0").unwrap());

        verify_base_package_iterator_pagination(paths, PACKAGE_INDEX_CHUNK_SIZE_MAX).await;
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn base_package_index_iterator_paginates_longest_entries() {
        let names = ('a'..='z')
            .map(|c| std::iter::repeat(c).take(PackagePath::MAX_NAME_BYTES).collect::<String>())
            .cycle();
        let paths = names.map(|name| PackagePath::from_name_and_variant(name, "0").unwrap());

        verify_base_package_iterator_pagination(paths, PACKAGE_INDEX_CHUNK_SIZE_MIN).await;
    }

    async fn verify_base_package_iterator_pagination(
        paths: impl Iterator<Item = PackagePath>,
        expected_chunk_size: usize,
    ) {
        let static_packages =
            paths.zip(HashRangeFull::default()).take(expected_chunk_size * 2).collect();
        let static_packages = Arc::new(StaticPackages::from_entries(static_packages));

        let (iter, stream) =
            fidl::endpoints::create_proxy_and_stream::<PackageIndexIteratorMarker>().unwrap();
        let task = Task::local(serve_base_package_index(static_packages, stream));

        let chunk = iter.next().await.unwrap();
        assert_eq!(chunk.len(), expected_chunk_size);

        let chunk = iter.next().await.unwrap();
        assert_eq!(chunk.len(), expected_chunk_size);

        let chunk = iter.next().await.unwrap();
        assert_eq!(chunk.len(), 0);

        let () = task.await;
    }
}

#[cfg(test)]
mod serve_needed_blobs_tests {
    use {
        super::*,
        fidl_fuchsia_io::FileMarker,
        fidl_fuchsia_pkg::{BlobInfoIteratorMarker, BlobInfoIteratorProxy, NeededBlobsProxy},
        fuchsia_hash::HashRangeFull,
        matches::assert_matches,
        std::collections::BTreeSet,
    };

    #[fuchsia_async::run_singlethreaded(test)]
    async fn start_stop() {
        let (_, stream) = fidl::endpoints::create_proxy_and_stream::<NeededBlobsMarker>().unwrap();

        let meta_blob_info = BlobInfo { blob_id: [0; 32].into(), length: 0 };

        let (pkgfs_install, _) = pkgfs::install::Client::new_test();
        let (pkgfs_needs, _) = pkgfs::needs::Client::new_test();

        assert_matches!(
            serve_needed_blobs(stream, meta_blob_info, &pkgfs_install, &pkgfs_needs).await,
            Err(ServeNeededBlobsError::UnexpectedClose)
        );
    }

    fn spawn_serve_needed_blobs_with_mocks(
        meta_blob_info: BlobInfo,
    ) -> (
        Task<Result<(), ServeNeededBlobsError>>,
        NeededBlobsProxy,
        pkgfs::install::Mock,
        pkgfs::needs::Mock,
    ) {
        let (proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<NeededBlobsMarker>().unwrap();

        let (pkgfs_install, pkgfs_install_mock) = pkgfs::install::Client::new_mock();
        let (pkgfs_needs, pkgfs_needs_mock) = pkgfs::needs::Client::new_mock();

        (
            Task::spawn(async move {
                serve_needed_blobs(stream, meta_blob_info, &pkgfs_install, &pkgfs_needs).await
            }),
            proxy,
            pkgfs_install_mock,
            pkgfs_needs_mock,
        )
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn expects_open_meta_blob() {
        let meta_blob_info = BlobInfo { blob_id: [0; 32].into(), length: 0 };

        let (task, proxy, pkgfs_install, pkgfs_needs) =
            spawn_serve_needed_blobs_with_mocks(meta_blob_info);

        let (iter, iter_server_end) =
            fidl::endpoints::create_proxy::<BlobInfoIteratorMarker>().unwrap();
        proxy.get_missing_blobs(iter_server_end).unwrap();
        assert_matches!(iter.next().await, Err(_));

        assert_matches!(
            task.await,
            Err(ServeNeededBlobsError::UnexpectedRequest {
                received: "get_missing_blobs",
                expected: "open_meta_blob"
            })
        );
        pkgfs_install.expect_done().await;
        pkgfs_needs.expect_done().await;
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn expects_open_meta_blob_once() {
        let meta_blob_info = BlobInfo { blob_id: [0; 32].into(), length: 0 };
        let (task, proxy, mut pkgfs_install, pkgfs_needs) =
            spawn_serve_needed_blobs_with_mocks(meta_blob_info);

        // Open a needed meta FAR blob and write it.
        let ((), ()) = future::join(
            async {
                pkgfs_install
                    .expect_create_blob([0; 32].into(), BlobKind::Package.into())
                    .await
                    .expect_payload(&[])
                    .await;
            },
            async {
                let (blob, blob_server_end) =
                    fidl::endpoints::create_proxy::<FileMarker>().unwrap();

                assert_matches!(proxy.open_meta_blob(blob_server_end).await, Ok(Ok(true)));

                let _ = blob.truncate(0).await;
                let _ = blob.write(&mut []).await;
                let _ = blob.close().await;
            },
        )
        .await;

        // Trying to open the meta FAR blob again after writing it successfully is a protocol violation.
        let (_blob, blob_server_end) = fidl::endpoints::create_proxy::<FileMarker>().unwrap();
        assert_matches!(proxy.open_meta_blob(blob_server_end).await, Err(_));

        assert_matches!(
            task.await,
            Err(ServeNeededBlobsError::UnexpectedRequest {
                received: "open_meta_blob",
                expected: "get_missing_blobs"
            })
        );
        pkgfs_install.expect_done().await;
        pkgfs_needs.expect_done().await;
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn handles_present_meta_blob() {
        let meta_blob_info = BlobInfo { blob_id: [0; 32].into(), length: 0 };
        let (task, proxy, mut pkgfs_install, pkgfs_needs) =
            spawn_serve_needed_blobs_with_mocks(meta_blob_info);

        // Try to open the meta FAR blob, but report it is no longer needed.
        let ((), ()) = future::join(
            async {
                pkgfs_install
                    .expect_create_blob([0; 32].into(), BlobKind::Package.into())
                    .await
                    .fail_open_with_already_exists();
            },
            async {
                let (blob, blob_server_end) =
                    fidl::endpoints::create_proxy::<FileMarker>().unwrap();

                assert_matches!(proxy.open_meta_blob(blob_server_end).await, Ok(Ok(false)));
                assert_matches!(blob.truncate(0).await, Err(_));
            },
        )
        .await;

        // Trying to open the meta FAR blob again after being told it is not needed is a protocol
        // violation.
        let (_blob, blob_server_end) = fidl::endpoints::create_proxy::<FileMarker>().unwrap();
        assert_matches!(proxy.open_meta_blob(blob_server_end).await, Err(_));

        assert_matches!(
            task.await,
            Err(ServeNeededBlobsError::UnexpectedRequest {
                received: "open_meta_blob",
                expected: "get_missing_blobs"
            })
        );
        pkgfs_install.expect_done().await;
        pkgfs_needs.expect_done().await;
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn allows_retrying_nonfatal_open_meta_blob_errors() {
        let meta_blob_info = BlobInfo { blob_id: [0; 32].into(), length: 1 };
        let (task, proxy, mut pkgfs_install, pkgfs_needs) =
            spawn_serve_needed_blobs_with_mocks(meta_blob_info);

        // Try to open the meta FAR blob, but report it is already being written concurrently.
        let ((), ()) = future::join(
            async {
                pkgfs_install
                    .expect_create_blob([0; 32].into(), BlobKind::Package.into())
                    .await
                    .fail_open_with_concurrent_write();
            },
            async {
                let (blob, blob_server_end) =
                    fidl::endpoints::create_proxy::<FileMarker>().unwrap();

                assert_matches!(
                    proxy.open_meta_blob(blob_server_end).await,
                    Ok(Err(fidl_fuchsia_pkg::OpenBlobError::ConcurrentWrite))
                );
                assert_matches!(blob.truncate(1).await, Err(_));
            },
        )
        .await;

        // Try to write the meta FAR blob, but report the written contents are corrupt.
        let ((), ()) = future::join(
            async {
                pkgfs_install
                    .expect_create_blob([0; 32].into(), BlobKind::Package.into())
                    .await
                    .fail_write_with_corrupt()
                    .await;
            },
            async {
                let (blob, blob_server_end) =
                    fidl::endpoints::create_proxy::<FileMarker>().unwrap();

                assert_matches!(proxy.open_meta_blob(blob_server_end).await, Ok(Ok(true)));

                let _ = blob.truncate(1).await;
                let _ = blob.write(&mut [0]).await;
                let _ = blob.close().await;
            },
        )
        .await;

        // Operation succeeds after pkgfs cooperates.
        let ((), ()) = future::join(
            async {
                pkgfs_install
                    .expect_create_blob([0; 32].into(), BlobKind::Package.into())
                    .await
                    .expect_payload(&[0])
                    .await;
            },
            async {
                let (blob, blob_server_end) =
                    fidl::endpoints::create_proxy::<FileMarker>().unwrap();

                assert_matches!(proxy.open_meta_blob(blob_server_end).await, Ok(Ok(true)));

                let _ = blob.truncate(1).await;
                let _ = blob.write(&mut [0]).await;
                let _ = blob.close().await;
            },
        )
        .await;

        // Task moves to next state after retried write operation succeeds.
        let (_blob, blob_server_end) = fidl::endpoints::create_proxy::<FileMarker>().unwrap();
        assert_matches!(proxy.open_meta_blob(blob_server_end).await, Err(_));
        assert_matches!(
            task.await,
            Err(ServeNeededBlobsError::UnexpectedRequest {
                received: "open_meta_blob",
                expected: "get_missing_blobs"
            })
        );
        pkgfs_install.expect_done().await;
        pkgfs_needs.expect_done().await;
    }

    async fn write_meta_blob(
        proxy: &NeededBlobsProxy,
        pkgfs_install: &mut pkgfs::install::Mock,
        meta_blob_info: BlobInfo,
    ) {
        let ((), ()) = future::join(
            async {
                pkgfs_install
                    .expect_create_blob(meta_blob_info.blob_id.into(), BlobKind::Package.into())
                    .await
                    .fail_open_with_already_exists();
            },
            async {
                let (_blob, blob_server_end) =
                    fidl::endpoints::create_proxy::<FileMarker>().unwrap();

                assert_matches!(proxy.open_meta_blob(blob_server_end).await, Ok(Ok(false)));
            },
        )
        .await;
    }

    async fn collect_blob_info_iterator(proxy: BlobInfoIteratorProxy) -> Vec<BlobInfo> {
        let mut res = vec![];

        loop {
            let chunk = proxy.next().await.unwrap();

            if chunk.is_empty() {
                break;
            }

            res.extend(chunk.into_iter().map(BlobInfo::from));
        }

        res
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn discovers_and_reports_missing_blobs() {
        let meta_blob_info = BlobInfo { blob_id: [0; 32].into(), length: 0 };
        let (task, proxy, mut pkgfs_install, mut pkgfs_needs) =
            spawn_serve_needed_blobs_with_mocks(meta_blob_info);

        write_meta_blob(&proxy, &mut pkgfs_install, meta_blob_info).await;

        let expected = HashRangeFull::default().take(2000).collect::<Vec<_>>();

        let ((), ()) = future::join(
            async {
                pkgfs_needs
                    .expect_enumerate_needs([0; 32].into())
                    .await
                    .enumerate_needs(
                        expected.iter().cloned().map(Into::into).collect::<BTreeSet<_>>(),
                    )
                    .await;
            },
            async {
                let (missing_blobs_iter, missing_blobs_iter_server_end) =
                    fidl::endpoints::create_proxy::<BlobInfoIteratorMarker>().unwrap();

                assert_matches!(proxy.get_missing_blobs(missing_blobs_iter_server_end), Ok(()));

                let missing_blobs = collect_blob_info_iterator(missing_blobs_iter).await;

                let expected = expected
                    .iter()
                    .cloned()
                    .map(|hash| BlobInfo { blob_id: hash.into(), length: 0 })
                    .collect::<Vec<_>>();
                assert_eq!(missing_blobs, expected);
            },
        )
        .await;

        drop(proxy);
        assert_matches!(task.await, Err(ServeNeededBlobsError::UnexpectedClose));
        pkgfs_install.expect_done().await;
        pkgfs_needs.expect_done().await;
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn handles_no_missing_blobs() {
        let meta_blob_info = BlobInfo { blob_id: [0; 32].into(), length: 0 };
        let (task, proxy, mut pkgfs_install, mut pkgfs_needs) =
            spawn_serve_needed_blobs_with_mocks(meta_blob_info);

        write_meta_blob(&proxy, &mut pkgfs_install, meta_blob_info).await;

        let ((), ()) = future::join(
            async {
                pkgfs_needs
                    .expect_enumerate_needs([0; 32].into())
                    .await
                    .fail_open_with_not_found()
                    .await;
            },
            async {
                let (missing_blobs_iter, missing_blobs_iter_server_end) =
                    fidl::endpoints::create_proxy::<BlobInfoIteratorMarker>().unwrap();

                assert_matches!(proxy.get_missing_blobs(missing_blobs_iter_server_end), Ok(()));

                let missing_blobs = collect_blob_info_iterator(missing_blobs_iter).await;

                assert_eq!(missing_blobs, vec![]);
            },
        )
        .await;

        // TODO(64622) Verify that the NeededBlobs protocol shuts down at this point (will be
        // implemented when implementing ExpectOpenContentBlob).

        drop(proxy);
        assert_matches!(task.await, Err(ServeNeededBlobsError::UnexpectedClose));
        pkgfs_install.expect_done().await;
        pkgfs_needs.expect_done().await;
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn fails_on_needs_enumeration_error() {
        let meta_blob_info = BlobInfo { blob_id: [0; 32].into(), length: 0 };
        let (task, proxy, mut pkgfs_install, mut pkgfs_needs) =
            spawn_serve_needed_blobs_with_mocks(meta_blob_info);

        write_meta_blob(&proxy, &mut pkgfs_install, meta_blob_info).await;

        let ((), ()) = future::join(
            async {
                pkgfs_needs
                    .expect_enumerate_needs([0; 32].into())
                    .await
                    .fail_open_with_unexpected_error()
                    .await;
            },
            async {
                let (missing_blobs_iter, missing_blobs_iter_server_end) =
                    fidl::endpoints::create_proxy::<BlobInfoIteratorMarker>().unwrap();

                assert_matches!(proxy.get_missing_blobs(missing_blobs_iter_server_end), Ok(()));

                assert_matches!(
                    missing_blobs_iter.next().await,
                    Err(fidl::Error::ClientChannelClosed { status: Status::PEER_CLOSED, .. })
                );
            },
        )
        .await;

        drop(proxy);
        assert_matches!(task.await, Err(ServeNeededBlobsError::ListNeeds(_)));
        pkgfs_install.expect_done().await;
        pkgfs_needs.expect_done().await;
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn dropping_needed_blobs_stops_missing_blob_iterator() {
        let meta_blob_info = BlobInfo { blob_id: [0; 32].into(), length: 0 };
        let (task, proxy, mut pkgfs_install, mut pkgfs_needs) =
            spawn_serve_needed_blobs_with_mocks(meta_blob_info);

        write_meta_blob(&proxy, &mut pkgfs_install, meta_blob_info).await;

        let ((), ()) = future::join(
            async {
                pkgfs_needs
                    .expect_enumerate_needs([0; 32].into())
                    .await
                    .enumerate_needs(HashRangeFull::default().take(10).collect())
                    .await;
            },
            async {
                let (missing_blobs_iter, missing_blobs_iter_server_end) =
                    fidl::endpoints::create_proxy::<BlobInfoIteratorMarker>().unwrap();

                assert_matches!(proxy.get_missing_blobs(missing_blobs_iter_server_end), Ok(()));

                // Closing the needed blobs request stream terminates any spawned tasks.
                drop(proxy);
                assert_matches!(
                    missing_blobs_iter.next().await,
                    Err(fidl::Error::ClientChannelClosed { status: Status::PEER_CLOSED, .. })
                );
            },
        )
        .await;

        assert_matches!(task.await, Err(ServeNeededBlobsError::UnexpectedClose));
        pkgfs_install.expect_done().await;
        pkgfs_needs.expect_done().await;
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn expects_get_missing_blobs_once() {
        let meta_blob_info = BlobInfo { blob_id: [0; 32].into(), length: 0 };
        let (task, proxy, mut pkgfs_install, mut pkgfs_needs) =
            spawn_serve_needed_blobs_with_mocks(meta_blob_info);

        write_meta_blob(&proxy, &mut pkgfs_install, meta_blob_info).await;

        // Enumerate the needs successfully once.
        let ((), ()) = future::join(
            async {
                pkgfs_needs
                    .expect_enumerate_needs([0; 32].into())
                    .await
                    .enumerate_needs(HashRangeFull::default().take(10).collect())
                    .await;
            },
            async {
                let (missing_blobs_iter, missing_blobs_iter_server_end) =
                    fidl::endpoints::create_proxy::<BlobInfoIteratorMarker>().unwrap();

                assert_matches!(proxy.get_missing_blobs(missing_blobs_iter_server_end), Ok(()));

                collect_blob_info_iterator(missing_blobs_iter).await;
            },
        )
        .await;

        // Trying to enumerate the missing blobs again is a protocol violation.
        let (_missing_blobs_iter, missing_blobs_iter_server_end) =
            fidl::endpoints::create_proxy::<BlobInfoIteratorMarker>().unwrap();
        assert_matches!(proxy.get_missing_blobs(missing_blobs_iter_server_end), Ok(()));

        assert_matches!(
            task.await,
            Err(ServeNeededBlobsError::UnexpectedRequest {
                received: "get_missing_blobs",
                expected: "open_blob"
            })
        );
        pkgfs_install.expect_done().await;
        pkgfs_needs.expect_done().await;
    }
}

#[cfg(test)]
mod serve_write_blob_tests {
    use {
        super::*,
        fidl_fuchsia_io::{FileMarker, FileProxy},
        futures::task::Poll,
        matches::assert_matches,
        proptest::prelude::*,
        proptest_derive::Arbitrary,
    };

    /// Calls the provided test function with an open File proxy being served by serve_write_blob
    /// and the corresponding request stream representing the open pkgfs install blob file.
    async fn do_serve_write_blob_with<F, Fut>(cb: F) -> Result<(), ServeWriteBlobError>
    where
        F: FnOnce(FileProxy, FileRequestStream) -> Fut,
        Fut: Future<Output = ()>,
    {
        let (pkgfs_blob, pkgfs_blob_stream) = OpenBlob::new_test(BlobKind::Data);

        let (pkg_cache_blob, pkg_cache_blob_stream) =
            fidl::endpoints::create_proxy_and_stream::<FileMarker>().unwrap();

        let task = serve_write_blob(pkg_cache_blob_stream, pkgfs_blob);
        let test = cb(pkg_cache_blob, pkgfs_blob_stream);

        let (res, ()) = future::join(task, test).await;
        res
    }

    /// Handles a single FIDL request on the provided stream, panicing if the received request is
    /// not the expected kind.
    macro_rules! serve_fidl_request {
        (
            $stream:expr, { $pat:pat => $handler:block, }
        ) => {
            match $stream.next().await.unwrap().unwrap() {
                $pat => $handler,
                req => panic!("unexpected request: {:?}", req),
            }
        };
    }

    /// Runs the provided FIDL request stream to compleation, running each handler in sequence,
    /// panicing if any incoming request is not the expected kind.
    macro_rules! serve_fidl_stream {
        (
            $stream:expr, { $( $pat:pat => $handler:block, )* }
        ) => {
            async move {
                $(
                    serve_fidl_request!($stream, { $pat => $handler, });
                )*
                assert_matches!($stream.next().await, None);
            }
        }
    }

    /// Sends a truncate request, asserts that the remote end receives the request, responds to the
    /// request, and asserts that the truncate request receives the expected mapped status code.
    async fn verify_truncate(
        proxy: &FileProxy,
        stream: &mut FileRequestStream,
        length: u64,
        pkgfs_response: Status,
    ) -> Status {
        let ((), o) = future::join(
            async move {
                serve_fidl_request!(stream, {
                    FileRequest::Truncate { length: actual_length, responder } => {
                        assert_eq!(length, actual_length);
                        responder.send(pkgfs_response.into_raw()).unwrap();
                    },
                });
            },
            async move { proxy.truncate(length).await.map(Status::from_raw).unwrap() },
        )
        .await;
        o
    }

    /// Sends a write request, asserts that the remote end receives the request, responds to the
    /// request, and asserts that the write request receives the expected mapped status code/length.
    async fn verify_write(
        proxy: &FileProxy,
        stream: &mut FileRequestStream,
        data: &[u8],
        pkgfs_response: Status,
    ) -> Status {
        let ((), o) = future::join(
            async move {
                serve_fidl_request!(stream, {
                    FileRequest::Write{ data: actual_data, responder } => {
                        assert_eq!(data, actual_data);
                        responder.send(pkgfs_response.into_raw(), data.len() as u64).unwrap();
                    },
                });
            },
            async move {
                let (s, len) =
                    proxy.write(data).await.map(|(s, len)| (Status::from_raw(s), len)).unwrap();
                if s == Status::OK {
                    assert_eq!(len, data.len() as u64);
                }
                s
            },
        )
        .await;
        o
    }

    /// Verify that closing the proxy results in the pkgfs backing file being explicitly closed.
    async fn verify_inner_blob_closes(proxy: FileProxy, mut stream: FileRequestStream) {
        drop(proxy);
        serve_fidl_stream!(stream, {
            FileRequest::Close { responder } => {
                responder.send(Status::OK.into_raw()).unwrap();
            },
        })
        .await;
    }

    /// Verify that an explicit close() request is proxied through to the pkgfs backing file.
    async fn verify_explicit_close(proxy: FileProxy, mut stream: FileRequestStream) {
        let ((), ()) = future::join(
            serve_fidl_stream!(stream, {
                FileRequest::Close { responder } => {
                    responder.send(Status::OK.into_raw()).unwrap();
                },
            }),
            async move {
                let _ = proxy.close().await;
                drop(proxy);
            },
        )
        .await;
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn start_stop() {
        let res = do_serve_write_blob_with(|proxy, stream| async move {
            drop(proxy);
            drop(stream);
        })
        .await;
        assert_matches!(res, Err(ServeWriteBlobError::UnexpectedClose));
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn happy_path_succeeds() {
        do_serve_write_blob_with(|proxy, mut stream| async move {
            assert_eq!(verify_truncate(&proxy, &mut stream, 200, Status::OK).await, Status::OK);
            assert_eq!(verify_write(&proxy, &mut stream, &[1; 100], Status::OK).await, Status::OK);
            assert_eq!(verify_write(&proxy, &mut stream, &[2; 100], Status::OK).await, Status::OK);
            verify_explicit_close(proxy, stream).await;
        })
        .await
        .unwrap();
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn happy_path_implicit_close_succeeds() {
        do_serve_write_blob_with(|proxy, mut stream| async move {
            assert_eq!(verify_truncate(&proxy, &mut stream, 200, Status::OK).await, Status::OK);
            assert_eq!(verify_write(&proxy, &mut stream, &[1; 100], Status::OK).await, Status::OK);
            assert_eq!(verify_write(&proxy, &mut stream, &[2; 100], Status::OK).await, Status::OK);
            verify_inner_blob_closes(proxy, stream).await;
        })
        .await
        .unwrap();
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn raises_out_of_space_during_truncate() {
        let res = do_serve_write_blob_with(|proxy, mut stream| async move {
            assert_eq!(
                verify_truncate(&proxy, &mut stream, 100, Status::NO_SPACE).await,
                Status::NO_SPACE
            );
            verify_inner_blob_closes(proxy, stream).await;
        })
        .await;
        assert_matches!(res, Err(ServeWriteBlobError::NoSpace));
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn truncate_maps_unknown_errors_to_internal() {
        let res = do_serve_write_blob_with(|proxy, mut stream| async move {
            assert_eq!(
                verify_truncate(&proxy, &mut stream, 100, Status::ADDRESS_UNREACHABLE).await,
                Status::INTERNAL
            );
            verify_inner_blob_closes(proxy, stream).await;
        })
        .await;
        assert_matches!(res, Err(ServeWriteBlobError::Truncate(_)));
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn raises_out_of_space_during_write() {
        let res = do_serve_write_blob_with(|proxy, mut stream| async move {
            assert_eq!(verify_truncate(&proxy, &mut stream, 100, Status::OK).await, Status::OK);
            assert_eq!(
                verify_write(&proxy, &mut stream, &[0; 1], Status::NO_SPACE).await,
                Status::NO_SPACE
            );
            verify_inner_blob_closes(proxy, stream).await;
        })
        .await;
        assert_matches!(res, Err(ServeWriteBlobError::NoSpace));
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn raises_corrupt_during_last_write() {
        let res = do_serve_write_blob_with(|proxy, mut stream| async move {
            assert_eq!(verify_truncate(&proxy, &mut stream, 10, Status::OK).await, Status::OK);
            assert_eq!(verify_write(&proxy, &mut stream, &[0; 5], Status::OK).await, Status::OK);
            assert_eq!(
                verify_write(&proxy, &mut stream, &[1; 5], Status::IO_DATA_INTEGRITY).await,
                Status::IO_DATA_INTEGRITY
            );
            verify_inner_blob_closes(proxy, stream).await;
        })
        .await;
        assert_matches!(res, Err(ServeWriteBlobError::Corrupt));
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn write_maps_unknown_errors_to_internal() {
        let res = do_serve_write_blob_with(|proxy, mut stream| async move {
            assert_eq!(verify_truncate(&proxy, &mut stream, 100, Status::OK).await, Status::OK);
            assert_eq!(
                verify_write(&proxy, &mut stream, &[1; 1], Status::ADDRESS_UNREACHABLE).await,
                Status::INTERNAL
            );
            verify_inner_blob_closes(proxy, stream).await;
        })
        .await;
        assert_matches!(res, Err(ServeWriteBlobError::Write(_)));
    }

    #[test]
    fn close_closes_inner_blob_first() {
        let mut executor = fuchsia_async::Executor::new().unwrap();

        let (pkgfs_blob, mut pkgfs_blob_stream) = OpenBlob::new_test(BlobKind::Data);

        let (pkg_cache_blob, pkg_cache_blob_stream) =
            fidl::endpoints::create_proxy_and_stream::<FileMarker>().unwrap();

        let task = serve_write_blob(pkg_cache_blob_stream, pkgfs_blob);
        futures::pin_mut!(task);

        let mut close_fut = pkg_cache_blob.close();
        drop(pkg_cache_blob);

        // Let the task process the close request, ensuring the close_future doesn't yet complete.
        assert_matches!(executor.run_until_stalled(&mut task), Poll::Pending);
        assert_matches!(executor.run_until_stalled(&mut close_fut), Poll::Pending);

        // Verify the inner blob is bineg closed.
        let () = executor.run_singlethreaded(async {
            serve_fidl_request!(pkgfs_blob_stream, {
                FileRequest::Close { responder } => {
                    responder.send(Status::OK.into_raw()).unwrap();
                },
            })
        });

        // Now that the inner blob is closed, the proxy task and close request can complete
        assert_matches!(
            executor.run_until_stalled(&mut task),
            Poll::Ready(Err(ServeWriteBlobError::UnexpectedClose))
        );
        assert_matches!(executor.run_until_stalled(&mut close_fut), Poll::Ready(Ok(0)));
    }

    #[derive(Debug, Clone, Copy, PartialEq, Eq, Arbitrary)]
    enum StubRequestor {
        Clone,
        Describe,
        Sync,
        GetAttr,
        SetAttr,
        NodeGetFlags,
        NodeSetFlags,
        Write,
        WriteAt,
        Read,
        ReadAt,
        Seek,
        Truncate,
        GetFlags,
        SetFlags,
        GetBuffer,
        // New API that references fuchsia.io2. Not strictly necessary to verify all possible
        // ordinals (which is the space of a u64 anyway).
        // AdvisoryLock

        // Always allowed.
        // Close
    }

    impl StubRequestor {
        fn method_name(self) -> &'static str {
            match self {
                StubRequestor::Clone => "clone",
                StubRequestor::Describe => "describe",
                StubRequestor::Sync => "sync",
                StubRequestor::GetAttr => "get_attr",
                StubRequestor::SetAttr => "set_attr",
                StubRequestor::NodeGetFlags => "node_get_flags",
                StubRequestor::NodeSetFlags => "node_set_flags",
                StubRequestor::Write => "write",
                StubRequestor::WriteAt => "write_at",
                StubRequestor::Read => "read",
                StubRequestor::ReadAt => "read_at",
                StubRequestor::Seek => "seek",
                StubRequestor::Truncate => "truncate",
                StubRequestor::GetFlags => "get_flags",
                StubRequestor::SetFlags => "set_flags",
                StubRequestor::GetBuffer => "get_buffer",
            }
        }

        fn make_stub_request(self, proxy: &FileProxy) -> impl Future<Output = ()> {
            use fidl::encoding::Decodable;
            match self {
                StubRequestor::Clone => {
                    let (_, server_end) =
                        fidl::endpoints::create_proxy::<fidl_fuchsia_io::NodeMarker>().unwrap();
                    let _ = proxy.clone(0, server_end);
                    future::ready(()).boxed()
                }
                StubRequestor::Describe => proxy.describe().map(|_| ()).boxed(),
                StubRequestor::Sync => proxy.sync().map(|_| ()).boxed(),
                StubRequestor::GetAttr => proxy.get_attr().map(|_| ()).boxed(),
                StubRequestor::SetAttr => proxy
                    .set_attr(0, &mut fidl_fuchsia_io::NodeAttributes::new_empty())
                    .map(|_| ())
                    .boxed(),
                StubRequestor::NodeGetFlags => proxy.node_get_flags().map(|_| ()).boxed(),
                StubRequestor::NodeSetFlags => proxy.node_set_flags(0).map(|_| ()).boxed(),
                StubRequestor::Write => proxy.write(&[0; 0]).map(|_| ()).boxed(),
                StubRequestor::WriteAt => proxy.write_at(&[0; 0], 0).map(|_| ()).boxed(),
                StubRequestor::Read => proxy.read(0).map(|_| ()).boxed(),
                StubRequestor::ReadAt => proxy.read_at(0, 0).map(|_| ()).boxed(),
                StubRequestor::Seek => {
                    proxy.seek(0, fidl_fuchsia_io::SeekOrigin::Start).map(|_| ()).boxed()
                }
                StubRequestor::Truncate => proxy.truncate(0).map(|_| ()).boxed(),
                StubRequestor::GetFlags => proxy.get_flags().map(|_| ()).boxed(),
                StubRequestor::SetFlags => proxy.set_flags(0).map(|_| ()).boxed(),
                StubRequestor::GetBuffer => proxy.get_buffer(0).map(|_| ()).boxed(),
            }
        }
    }

    #[derive(Debug, Clone, Copy, PartialEq, Eq, Arbitrary)]
    enum InitialState {
        ExpectTruncate,
        ExpectWrite,
        ExpectClose,
    }

    impl InitialState {
        fn expected_method_name(self) -> &'static str {
            match self {
                InitialState::ExpectTruncate => "truncate",
                InitialState::ExpectWrite => "write",
                InitialState::ExpectClose => "close",
            }
        }

        async fn enter(self, proxy: &FileProxy, stream: &mut FileRequestStream) {
            match self {
                InitialState::ExpectTruncate => {}
                InitialState::ExpectWrite => {
                    assert_eq!(verify_truncate(proxy, stream, 100, Status::OK).await, Status::OK);
                }
                InitialState::ExpectClose => {
                    assert_eq!(verify_truncate(proxy, stream, 100, Status::OK).await, Status::OK);
                    assert_eq!(
                        verify_write(proxy, stream, &[0; 100], Status::OK).await,
                        Status::OK
                    );
                }
            }
        }
    }

    proptest! {
        // Failure seed persistence isn't working in Fuchsia tests, and these tests are expected to
        // verify the entire input space anyway. Enable result caching to skip running the same
        // case more than once.
        #![proptest_config(ProptestConfig{
            failure_persistence: None,
            result_cache: proptest::test_runner::basic_result_cache,
            ..Default::default()
        })]

        #[test]
        fn allows_close_in_any_state(initial_state: InitialState) {
            let mut executor = fuchsia_async::Executor::new().unwrap();
            let () = executor.run_singlethreaded(async move {

                let res = do_serve_write_blob_with(|proxy, mut stream| async move {
                    initial_state.enter(&proxy, &mut stream).await;
                    verify_explicit_close(proxy, stream).await;
                })
                .await;

                match initial_state {
                    InitialState::ExpectClose => assert_matches!(res, Ok(())),
                    _ => assert_matches!(res, Err(ServeWriteBlobError::UnexpectedClose)),
                }
            });
        }

        #[test]
        fn rejects_unexpected_requests(initial_state: InitialState, bad_request: StubRequestor) {
            // Skip stub requests that are the expected request for this initial state.
            prop_assume!(initial_state.expected_method_name() != bad_request.method_name());

            let mut executor = fuchsia_async::Executor::new().unwrap();
            let () = executor.run_singlethreaded(async move {

                let res = do_serve_write_blob_with(|proxy, mut stream| async move {
                    initial_state.enter(&proxy, &mut stream).await;

                    let bad_request_fut = bad_request.make_stub_request(&proxy);

                    let ((), ()) = future::join(
                        async move {
                            let _ = bad_request_fut.await;
                        },
                        verify_inner_blob_closes(proxy, stream),
                    )
                    .await;
                })
                .await;

                match res {
                    Err(ServeWriteBlobError::UnexpectedRequest{ received, expected }) => {
                        prop_assert_eq!(received, bad_request.method_name());
                        prop_assert_eq!(expected, initial_state.expected_method_name());
                    }
                    res => panic!("Expected UnexpectedRequest error, got {:?}", res),
                }
                Ok(())
            })?;
        }
    }
}
