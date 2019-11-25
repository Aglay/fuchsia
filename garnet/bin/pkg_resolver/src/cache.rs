// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{queue, repository_manager::Stats},
    failure::Fail,
    fidl::endpoints::ServerEnd,
    fidl_fuchsia_amber::OpenedRepositoryProxy,
    fidl_fuchsia_io::{
        DirectoryEvent, DirectoryMarker, DirectoryProxy, FileEvent, FileMarker, FileProxy, NodeInfo,
    },
    fidl_fuchsia_pkg::PackageCacheProxy,
    fidl_fuchsia_pkg_ext::{BlobId, BlobIdParseError, MirrorConfig, RepositoryConfig},
    files_async::{readdir, DirentKind},
    fuchsia_syslog::{fx_log_err, fx_log_info, fx_log_warn},
    fuchsia_url::pkg_url::PkgUrl,
    fuchsia_zircon::Status,
    futures::{
        compat::{Future01CompatExt, Stream01CompatExt},
        prelude::*,
        stream::FuturesUnordered,
    },
    http::Uri,
    hyper::{body::Payload, Body, Request, StatusCode},
    parking_lot::Mutex,
    std::{
        convert::TryInto,
        hash::Hash,
        num::TryFromIntError,
        sync::{
            atomic::{AtomicBool, Ordering},
            Arc,
        },
    },
};

mod retry;

pub type BlobFetcher = queue::WorkSender<BlobId, FetchBlobContext, Result<(), Arc<FetchError>>>;

/// Provides access to the package cache components.
#[derive(Clone)]
pub struct PackageCache {
    cache: PackageCacheProxy,
    pkgfs_install: DirectoryProxy,
    pkgfs_needs: DirectoryProxy,
}

impl PackageCache {
    /// Constructs a new [`PackageCache`].
    pub fn new(
        cache: PackageCacheProxy,
        pkgfs_install: DirectoryProxy,
        pkgfs_needs: DirectoryProxy,
    ) -> Self {
        Self { cache, pkgfs_install, pkgfs_needs }
    }

    /// Open the requested package by merkle root using the given selectors, serving the package
    /// directory on the given directory request on success.
    pub async fn open(
        &self,
        merkle: BlobId,
        selectors: &Vec<String>,
        dir_request: ServerEnd<DirectoryMarker>,
    ) -> Result<(), PackageOpenError> {
        let fut = self.cache.open(
            &mut merkle.into(),
            &mut selectors.iter().map(|s| s.as_str()),
            dir_request,
        );
        match Status::from_raw(fut.await?) {
            Status::OK => Ok(()),
            Status::NOT_FOUND => Err(PackageOpenError::NotFound),
            status => Err(PackageOpenError::UnexpectedStatus(status)),
        }
    }

    /// Check to see if a package with the given merkle root exists and is readable.
    pub async fn package_exists(&self, merkle: BlobId) -> Result<bool, PackageOpenError> {
        let (_dir, server_end) = fidl::endpoints::create_proxy()?;
        let selectors = vec![];
        match self.open(merkle, &selectors, server_end).await {
            Ok(()) => Ok(true),
            Err(PackageOpenError::NotFound) => Ok(false),
            Err(e) => Err(e),
        }
    }

    /// Create a new blob with the given install intent.
    ///
    /// Returns None if the blob already exists and is readable.
    async fn create_blob(
        &self,
        merkle: BlobId,
        blob_kind: BlobKind,
    ) -> Result<Option<FileProxy>, FuchsiaIoError> {
        let (file, server_end) =
            fidl::endpoints::create_proxy::<FileMarker>().map_err(FuchsiaIoError::CreateProxy)?;
        let server_end = ServerEnd::new(server_end.into_channel());

        let flags = fidl_fuchsia_io::OPEN_FLAG_CREATE
            | fidl_fuchsia_io::OPEN_RIGHT_WRITABLE
            | fidl_fuchsia_io::OPEN_FLAG_DESCRIBE;
        let path = blob_kind.make_install_path(&merkle);
        self.pkgfs_install
            .open(flags, 0, &path, server_end)
            .map_err(FuchsiaIoError::SendOpenRequest)?;

        let mut events = file.take_event_stream();
        let FileEvent::OnOpen_ { s: status, info } = events
            .next()
            .await
            .ok_or_else(|| FuchsiaIoError::EventStreamClosed)?
            .map_err(FuchsiaIoError::OnOpenDecode)?;

        let file = match Status::from_raw(status) {
            Status::OK => Ok(Some(file)),
            // Lost a race with another process writing to blobfs, and the blob already exists.
            // Or, we are the first one to try to create the empty blob which always exists.
            Status::ALREADY_EXISTS => Ok(None),
            status => Err(FuchsiaIoError::OpenError(status)),
        }?;

        match info.map(|info| *info) {
            Some(NodeInfo::File(_)) => Ok(file),
            Some(_) => Err(FuchsiaIoError::WrongNodeType),
            None => Err(FuchsiaIoError::ExpectedNodeInfo),
        }
    }

    /// Returns a stream of chunks of blobs that are needed to resolve the package specified by
    /// `pkg_merkle` provided that the `pkg_merkle` blob has previously been written to
    /// /pkgfs/install/pkg/. The package should be available in /pkgfs/versions when this stream
    /// terminates without error.
    fn list_needs(
        &self,
        pkg_merkle: BlobId,
    ) -> impl Stream<Item = Result<Vec<BlobId>, ListNeedsError>> + '_ {
        // None if stream is terminated and should not continue to enumerate needs.
        let state = Some(&self.pkgfs_needs);

        futures::stream::unfold(state, move |state: Option<&DirectoryProxy>| {
            async move {
                if let Some(pkgfs_needs) = state {
                    match enumerate_needs_dir(pkgfs_needs, pkg_merkle).await {
                        Ok(needs) => {
                            if needs.is_empty() {
                                None
                            } else {
                                Some((Ok(needs), Some(pkgfs_needs)))
                            }
                        }
                        // report the error and terminate the stream.
                        Err(err) => return Some((Err(err), None)),
                    }
                } else {
                    None
                }
            }
        })
    }
}

/// Lists all blobs currently in the `pkg_merkle`'s needs directory.
async fn enumerate_needs_dir(
    pkgfs_needs: &DirectoryProxy,
    pkg_merkle: BlobId,
) -> Result<Vec<BlobId>, ListNeedsError> {
    let (needs_dir, server_end) =
        fidl::endpoints::create_proxy::<DirectoryMarker>().map_err(FuchsiaIoError::CreateProxy)?;
    let server_end = ServerEnd::new(server_end.into_channel());

    let flags = fidl_fuchsia_io::OPEN_FLAG_DIRECTORY
        | fidl_fuchsia_io::OPEN_RIGHT_READABLE
        | fidl_fuchsia_io::OPEN_FLAG_DESCRIBE;
    pkgfs_needs
        .open(flags, 0, &format!("packages/{}", pkg_merkle.to_string()), server_end)
        .map_err(FuchsiaIoError::SendOpenRequest)?;

    let mut events = needs_dir.take_event_stream();
    let DirectoryEvent::OnOpen_ { s: status, info } = events
        .next()
        .await
        .ok_or_else(|| FuchsiaIoError::EventStreamClosed)?
        .map_err(FuchsiaIoError::OnOpenDecode)?;

    match Status::from_raw(status) {
        Status::OK => Ok(()),
        Status::NOT_FOUND => return Ok(vec![]),
        status => Err(FuchsiaIoError::OpenError(status)),
    }?;

    match info.map(|info| *info) {
        Some(NodeInfo::Directory(_)) => Ok(()),
        Some(_) => Err(FuchsiaIoError::WrongNodeType),
        None => Err(FuchsiaIoError::ExpectedNodeInfo),
    }?;

    let entries = readdir(&needs_dir).await.map_err(ListNeedsError::ReadDir)?;

    Ok(entries
        .into_iter()
        .filter_map(|entry| {
            if entry.kind == DirentKind::File {
                Some(entry.name.parse().map_err(ListNeedsError::ParseError))
            } else {
                fx_log_warn!("Ignoring unknown needs entry: {:?}", entry.name);
                None
            }
        })
        .collect::<Result<Vec<BlobId>, ListNeedsError>>()?)
}

#[derive(Debug, Fail)]
pub enum FuchsiaIoError {
    #[fail(display = "unable to create a fidl proxy: {}", _0)]
    CreateProxy(#[cause] fidl::Error),

    #[fail(display = "unable to send open request: {}", _0)]
    SendOpenRequest(#[cause] fidl::Error),

    #[fail(display = "node info not provided by server")]
    ExpectedNodeInfo,

    #[fail(display = "event stream closed prematurely")]
    EventStreamClosed,

    #[fail(display = "failed to read OnOpen event: {}", _0)]
    OnOpenDecode(#[cause] fidl::Error),

    #[fail(display = "open failed with status {}", _0)]
    OpenError(#[cause] Status),

    #[fail(display = "node was not the expected type")]
    WrongNodeType,
}

#[derive(Debug, Fail)]
pub enum PackageOpenError {
    #[fail(display = "fidl error: {}", _0)]
    Fidl(#[cause] fidl::Error),

    #[fail(display = "package not found")]
    NotFound,

    #[fail(display = "package cache returned unexpected status: {}", _0)]
    UnexpectedStatus(#[cause] Status),
}

impl From<fidl::Error> for PackageOpenError {
    fn from(err: fidl::Error) -> Self {
        Self::Fidl(err)
    }
}

impl From<PackageOpenError> for Status {
    fn from(x: PackageOpenError) -> Self {
        match x {
            PackageOpenError::NotFound => Status::NOT_FOUND,
            _ => Status::INTERNAL,
        }
    }
}

#[derive(Debug, Fail)]
pub enum ListNeedsError {
    #[fail(display = "io error while listing needs: {}", _0)]
    Io(#[cause] FuchsiaIoError),

    #[fail(display = "could not read needs dir: {}", _0)]
    ReadDir(#[cause] files_async::Error),

    #[fail(display = "unable to parse a need blob id: {}", _0)]
    ParseError(#[cause] BlobIdParseError),
}

impl From<FuchsiaIoError> for ListNeedsError {
    fn from(x: FuchsiaIoError) -> Self {
        ListNeedsError::Io(x)
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
enum BlobKind {
    Package,
    Data,
}

impl BlobKind {
    fn make_install_path(&self, merkle: &BlobId) -> String {
        let name = match *self {
            BlobKind::Package => "pkg",
            BlobKind::Data => "blob",
        };
        format!("{}/{}", name, merkle)
    }
}

pub async fn cache_package<'a>(
    repo: OpenedRepositoryProxy,
    config: &'a RepositoryConfig,
    url: &'a PkgUrl,
    cache: &'a PackageCache,
    blob_fetcher: &'a BlobFetcher,
) -> Result<BlobId, CacheError> {
    let (merkle, size) = merkle_for_url(&repo, url).await.map_err(CacheError::MerkleFor)?;

    // If a merkle pin was specified, use it, but only after having verified that the name and
    // variant exist in the TUF repo.  Note that this doesn't guarantee that the merkle pinned
    // package ever actually existed in the repo or that the merkle pin refers to the named
    // package.
    let merkle = if let Some(merkle_pin) = url.package_hash() {
        merkle_pin.parse().expect("package_hash() to always return a valid merkleroot")
    } else {
        merkle
    };

    // If the package already exists, we are done.
    if cache.package_exists(merkle).await.unwrap_or_else(|e| {
        fx_log_err!("unable to check if {} is already cached, assuming it isn't: {}", url, e);
        false
    }) {
        return Ok(merkle);
    }

    let mirrors = config.mirrors().to_vec().into();

    // Fetch the meta.far.
    blob_fetcher
        .push(
            merkle,
            FetchBlobContext {
                blob_kind: BlobKind::Package,
                mirrors: Arc::clone(&mirrors),
                expected_len: Some(size),
            },
        )
        .await
        .expect("processor exists")?;

    cache
        .list_needs(merkle)
        .err_into::<CacheError>()
        .try_for_each(|needs| {
            // Fetch the blobs with some amount of concurrency.
            fx_log_info!("Fetching blobs: {:#?}", needs);
            blob_fetcher
                .push_all(needs.into_iter().map(|need| {
                    (
                        need,
                        FetchBlobContext {
                            blob_kind: BlobKind::Data,
                            mirrors: Arc::clone(&mirrors),
                            expected_len: None,
                        },
                    )
                }))
                .collect::<FuturesUnordered<_>>()
                .map(|res| res.expect("processor exists"))
                .try_collect::<()>()
                .err_into()
        })
        .await?;

    Ok(merkle)
}

#[derive(Debug, Fail)]
pub enum CacheError {
    #[fail(display = "fidl error: {}", _0)]
    Fidl(#[cause] fidl::Error),

    #[fail(display = "while looking up merkle root for package: {}", _0)]
    MerkleFor(#[cause] MerkleForError),

    #[fail(display = "while listing needed blobs for package: {}", _0)]
    ListNeeds(#[cause] ListNeedsError),

    #[fail(display = "while fetching blobs for package: {}", _0)]
    Fetch(Arc<FetchError>),
}

impl From<ListNeedsError> for CacheError {
    fn from(x: ListNeedsError) -> Self {
        CacheError::ListNeeds(x)
    }
}

impl From<fidl::Error> for CacheError {
    fn from(x: fidl::Error) -> Self {
        Self::Fidl(x)
    }
}

impl From<Arc<FetchError>> for CacheError {
    fn from(x: Arc<FetchError>) -> Self {
        Self::Fetch(x)
    }
}

// From resolver.fidl:
// * `ZX_ERR_ACCESS_DENIED` if the resolver does not have permission to fetch a package blob.
// * `ZX_ERR_IO` if there is some other unspecified error during I/O.
// * `ZX_ERR_NOT_FOUND` if the package or a package blob does not exist.
// * `ZX_ERR_NO_SPACE` if there is no space available to store the package.
// * `ZX_ERR_UNAVAILABLE` if the resolver is currently unable to fetch a package blob.
impl CacheError {
    pub(crate) fn to_resolve_status(&self) -> Status {
        match self {
            CacheError::Fidl(_) => Status::IO,
            CacheError::MerkleFor(err) => err.to_resolve_status(),
            CacheError::ListNeeds(err) => err.to_resolve_status(),
            CacheError::Fetch(err) => err.to_resolve_status(),
        }
    }
}
impl MerkleForError {
    fn to_resolve_status(&self) -> Status {
        match self {
            MerkleForError::Fidl(_) => Status::IO,
            MerkleForError::NotFound => Status::NOT_FOUND,
            MerkleForError::UnexpectedStatus(_) => Status::INTERNAL,
            MerkleForError::ParseError(_) => Status::INTERNAL,
            MerkleForError::BlobTooLarge(_) => Status::INTERNAL,
        }
    }
}
impl ListNeedsError {
    fn to_resolve_status(&self) -> Status {
        match self {
            ListNeedsError::Io(_) => Status::IO,
            ListNeedsError::ReadDir(_) => Status::IO,
            ListNeedsError::ParseError(_) => Status::INTERNAL,
        }
    }
}
impl FetchError {
    fn to_resolve_status(&self) -> Status {
        match self {
            FetchError::CreateBlob(_) => Status::IO,
            FetchError::BadHttpStatus(hyper::StatusCode::UNAUTHORIZED) => Status::ACCESS_DENIED,
            FetchError::BadHttpStatus(hyper::StatusCode::FORBIDDEN) => Status::ACCESS_DENIED,
            FetchError::BadHttpStatus(_) => Status::UNAVAILABLE,
            FetchError::ContentLengthMismatch { .. } => Status::UNAVAILABLE,
            FetchError::UnknownLength => Status::UNAVAILABLE,
            FetchError::BlobTooSmall => Status::UNAVAILABLE,
            FetchError::BlobTooLarge => Status::UNAVAILABLE,
            FetchError::Hyper(_) => Status::UNAVAILABLE,
            FetchError::Http(_) => Status::UNAVAILABLE,
            FetchError::Overwrite => Status::IO,
            FetchError::Truncate(_) => Status::IO,
            FetchError::Fidl(_) => Status::IO,
            FetchError::Io(_) => Status::IO,
            FetchError::NoMirrors => Status::INTERNAL,
            FetchError::BlobUrl(_) => Status::INTERNAL,
        }
    }
}

async fn merkle_for_url<'a>(
    repo: &'a OpenedRepositoryProxy,
    url: &'a PkgUrl,
) -> Result<(BlobId, u64), MerkleForError> {
    let (status, message, merkle, size) =
        repo.merkle_for(url.name().unwrap(), url.variant()).await.map_err(MerkleForError::Fidl)?;
    match Status::ok(status) {
        Ok(()) => Ok(()),
        Err(Status::NOT_FOUND) => Err(MerkleForError::NotFound),
        Err(status) => {
            fx_log_err!("failed to lookup merkle for {}: {} {}", url, status, message);
            Err(MerkleForError::UnexpectedStatus(status))
        }
    }?;

    let merkle = merkle.parse().map_err(MerkleForError::ParseError)?;
    let size = size.try_into().map_err(MerkleForError::BlobTooLarge)?;

    Ok((merkle, size))
}

#[derive(Debug, Fail)]
pub enum MerkleForError {
    #[fail(display = "failed to query amber for merkle: {}", _0)]
    Fidl(#[cause] fidl::Error),

    #[fail(display = "the package was not found in the repository")]
    NotFound,

    #[fail(display = "amber returned an unexpected status: {}", _0)]
    UnexpectedStatus(#[cause] Status),

    #[fail(display = "amber returned an invalid merkle root: {}", _0)]
    ParseError(#[cause] BlobIdParseError),

    #[fail(display = "amber returned a blob size that was too large: {}", _0)]
    BlobTooLarge(#[cause] TryFromIntError),
}

#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct FetchBlobContext {
    blob_kind: BlobKind,
    mirrors: Arc<[MirrorConfig]>,
    expected_len: Option<u64>,
}

impl queue::TryMerge for FetchBlobContext {
    fn try_merge(&mut self, other: Self) -> Result<(), Self> {
        // Unmergeable if both contain different expected lengths. One of these instances will
        // fail, but we can't know which one here.
        let expected_len = match (self.expected_len, other.expected_len) {
            (Some(x), None) | (None, Some(x)) => Some(x),
            (None, None) => None,
            (Some(x), Some(y)) if x == y => Some(x),
            _ => return Err(other),
        };

        // Installing a blob as a package will fulfill any pending needs of that blob as a data
        // blob as well, so upgrade Data to Package.
        let blob_kind =
            if self.blob_kind == BlobKind::Package || other.blob_kind == BlobKind::Package {
                BlobKind::Package
            } else {
                BlobKind::Data
            };

        // For now, don't attempt to merge mirrors, but do merge these contexts if the mirrors are
        // equivalent.
        if self.mirrors != other.mirrors {
            return Err(other);
        }

        // Contexts are mergeable, apply the merged state.
        self.expected_len = expected_len;
        self.blob_kind = blob_kind;
        Ok(())
    }
}

pub fn make_blob_fetch_queue(
    cache: PackageCache,
    max_concurrency: usize,
    stats: Arc<Mutex<Stats>>,
) -> (impl Future<Output = ()>, BlobFetcher) {
    let http_client = Arc::new(fuchsia_hyper::new_https_client());

    let (blob_fetch_queue, blob_fetcher) =
        queue::work_queue(max_concurrency, move |merkle, context: FetchBlobContext| {
            let http_client = Arc::clone(&http_client);
            let cache = cache.clone();
            let stats = Arc::clone(&stats);

            async move {
                fetch_blob(
                    &*http_client,
                    &context.mirrors,
                    merkle,
                    context.blob_kind,
                    context.expected_len,
                    &cache,
                    stats,
                )
                .map_err(Arc::new)
                .await
            }
        });

    (blob_fetch_queue.into_future(), blob_fetcher)
}

async fn fetch_blob(
    client: &fuchsia_hyper::HttpsClient,
    mirrors: &[MirrorConfig],
    merkle: BlobId,
    blob_kind: BlobKind,
    expected_len: Option<u64>,
    cache: &PackageCache,
    stats: Arc<Mutex<Stats>>,
) -> Result<(), FetchError> {
    if mirrors.is_empty() {
        return Err(FetchError::NoMirrors);
    }

    // TODO try the other mirrors depending on the errors encountered trying this one.
    let blob_mirror_url = mirrors[0].blob_mirror_url();
    let mirror_stats = stats.lock().for_mirror(blob_mirror_url.to_owned());
    let blob_url = make_blob_url(blob_mirror_url, &merkle)?;

    let flaked = Arc::new(AtomicBool::new(false));

    fuchsia_backoff::retry_or_first_error(retry::blob_fetch(), || {
        let flaked = Arc::clone(&flaked);
        let mirror_stats = &mirror_stats;

        async {
            if let Some(blob) =
                cache.create_blob(merkle, blob_kind).await.map_err(FetchError::CreateBlob)?
            {
                download_blob(client, &blob_url, blob_kind, expected_len, blob).await?;
            }

            Ok(())
        }
        .inspect(move |res| match res.as_ref().map_err(FetchError::kind) {
            Err(FetchErrorKind::NetworkRateLimit) => {
                mirror_stats.network_rate_limits().increment();
            }
            Err(FetchErrorKind::Network) => {
                flaked.store(true, Ordering::SeqCst);
            }
            Err(FetchErrorKind::Other) => {}
            Ok(()) => {
                if flaked.load(Ordering::SeqCst) {
                    mirror_stats.network_blips().increment();
                }
            }
        })
    })
    .await
}

#[derive(Debug, Fail)]
pub enum BlobUrlError {
    #[fail(display = "mirror URI doesn't have a path")]
    UriWithoutPath,

    #[fail(display = "HTTP error: {}", _0)]
    Http(#[cause] http::Error),

    #[fail(display = "invalid URI: {}", _0)]
    InvalidUri(#[cause] http::uri::InvalidUri),

    #[fail(display = "invalid URI parts: {}", _0)]
    InvalidUriParts(#[cause] http::uri::InvalidUriParts),
}

impl From<http::Error> for BlobUrlError {
    fn from(x: http::Error) -> Self {
        BlobUrlError::Http(x)
    }
}

impl From<http::uri::InvalidUri> for BlobUrlError {
    fn from(x: http::uri::InvalidUri) -> Self {
        BlobUrlError::InvalidUri(x)
    }
}

impl From<http::uri::InvalidUriParts> for BlobUrlError {
    fn from(x: http::uri::InvalidUriParts) -> Self {
        BlobUrlError::InvalidUriParts(x)
    }
}

fn make_blob_url(blob_mirror_url: &str, merkle: &BlobId) -> Result<hyper::Uri, BlobUrlError> {
    let uri = blob_mirror_url.parse::<Uri>()?;

    let mut uri_parts = uri.into_parts();
    let (path, query) = match &uri_parts.path_and_query {
        Some(path_and_query) => {
            // Remove a trailing slash from path, if any.
            let mut modified_path = path_and_query.path().to_owned();
            if modified_path.ends_with('/') {
                modified_path.pop();
            }
            (modified_path, path_and_query.query())
        }
        None => return Err(BlobUrlError::UriWithoutPath),
    };
    // Add the merkle string to the end of the path.
    // There isn't a way to reconstruct a PathAndQuery by its struct members,
    // so we have to use format and then parse from a string...
    uri_parts.path_and_query = if let Some(query) = query {
        Some(format!("{}/{}?{}", path, &merkle, query).parse()?)
    } else {
        Some(format!("{}/{}", path, &merkle).parse()?)
    };

    Ok(Uri::from_parts(uri_parts)?)
}

async fn download_blob(
    client: &fuchsia_hyper::HttpsClient,
    uri: &http::Uri,
    blob_kind: BlobKind,
    expected_len: Option<u64>,
    dest: FileProxy,
) -> Result<(), FetchError> {
    // If dest is not `Close`d after a partial write or a write of corrupted bytes,
    // subsequent attempts to re-open the blob for writing (e.g. if the package resolve
    // is re-tried) will fail. Suspected that pkgfs is not closing its channel to blobfs
    // for the given blob when the resolver's channel to pkgfs is closed,
    // and keeping the channel open causes blobfs to fail attempts to open the same blob
    // again for writing.
    struct FileProxyCloserGuard<'a> {
        f: &'a FileProxy,
    }

    impl Drop for FileProxyCloserGuard<'_> {
        fn drop(&mut self) {
            // Sending the Close message is synchronous, only waiting for the response is async.
            let _f = self.f.close();
        }
    }

    let _fpc = FileProxyCloserGuard { f: &dest };

    let request = Request::get(uri).body(Body::empty())?;
    let response = client.request(request).compat().await?;

    if response.status() != StatusCode::OK {
        return Err(FetchError::BadHttpStatus(response.status()));
    }

    let body = response.into_body();

    let expected_len = match (expected_len, body.content_length()) {
        (Some(expected), Some(actual)) => {
            if expected != actual {
                return Err(FetchError::ContentLengthMismatch { expected, actual });
            } else {
                expected
            }
        }
        (Some(length), None) | (None, Some(length)) => length,
        (None, None) => return Err(FetchError::UnknownLength),
    };

    Status::ok(dest.truncate(expected_len).await.map_err(FetchError::Fidl)?)
        .map_err(FetchError::Truncate)?;

    let mut chunks = body.compat();
    let mut written = 0u64;
    while let Some(chunk) = chunks.try_next().await? {
        if written + chunk.len() as u64 > expected_len {
            return Err(FetchError::BlobTooLarge);
        }

        // Split this chunk into smaller chunks that can fit through FIDL.
        let mut chunk = chunk.as_ref();
        while !chunk.is_empty() {
            let subchunk_len = chunk.len().min(fidl_fuchsia_io::MAX_BUF as usize);
            let subchunk = &chunk[..subchunk_len];

            let fut = dest.write(&mut subchunk.into_iter().cloned());
            let (status, actual) = fut.await.map_err(FetchError::Fidl)?;
            match Status::from_raw(status) {
                Status::OK => {}
                Status::ALREADY_EXISTS => {
                    if blob_kind == BlobKind::Package && written + actual == expected_len {
                        // pkgfs returns ALREADY_EXISTS on the final write of a meta FAR iff no other
                        // needs exist. Allow the error, but ignore the hint and check needs anyway.
                    } else {
                        return Err(FetchError::Io(Status::ALREADY_EXISTS));
                    }
                }
                status => return Err(FetchError::Io(status)),
            }
            if actual > subchunk_len as u64 {
                return Err(FetchError::Overwrite);
            }

            written += actual;
            chunk = &chunk[actual as usize..];
        }
    }

    if expected_len != written {
        return Err(FetchError::BlobTooSmall);
    }

    Ok(())
}

#[derive(Debug, Fail)]
pub enum FetchError {
    #[fail(display = "could not create blob: {}", _0)]
    CreateBlob(#[cause] FuchsiaIoError),

    #[fail(display = "http request expected 200, got {}", _0)]
    BadHttpStatus(hyper::StatusCode),

    #[fail(display = "repository has no configured mirrors")]
    NoMirrors,

    #[fail(display = "expected blob length of {}, got {}", expected, actual)]
    ContentLengthMismatch { expected: u64, actual: u64 },

    #[fail(display = "blob length not known or provided by server")]
    UnknownLength,

    #[fail(display = "downloaded blob was too small")]
    BlobTooSmall,

    #[fail(display = "downloaded blob was too large")]
    BlobTooLarge,

    #[fail(display = "file endpoint reported more bytes written than were provided")]
    Overwrite,

    #[fail(display = "failed to truncate blob: {}", _0)]
    Truncate(#[cause] Status),

    #[fail(display = "hyper error: {}", _0)]
    Hyper(#[cause] hyper::Error),

    #[fail(display = "http error: {}", _0)]
    Http(#[cause] hyper::http::Error),

    #[fail(display = "fidl error: {}", _0)]
    Fidl(#[cause] fidl::Error),

    #[fail(display = "io error: {}", _0)]
    Io(#[cause] Status),

    #[fail(display = "blob url error: {}", _0)]
    BlobUrl(#[cause] BlobUrlError),
}

impl From<hyper::Error> for FetchError {
    fn from(x: hyper::Error) -> Self {
        FetchError::Hyper(x)
    }
}

impl From<hyper::http::Error> for FetchError {
    fn from(x: hyper::http::Error) -> Self {
        FetchError::Http(x)
    }
}

impl From<BlobUrlError> for FetchError {
    fn from(x: BlobUrlError) -> Self {
        FetchError::BlobUrl(x)
    }
}

impl FetchError {
    fn kind(&self) -> FetchErrorKind {
        match self {
            FetchError::BadHttpStatus(StatusCode::TOO_MANY_REQUESTS) => {
                FetchErrorKind::NetworkRateLimit
            }
            FetchError::Hyper(_) | FetchError::Http(_) | FetchError::BadHttpStatus(_) => {
                FetchErrorKind::Network
            }
            _ => FetchErrorKind::Other,
        }
    }
}

#[derive(Debug, PartialEq, Eq)]
pub enum FetchErrorKind {
    NetworkRateLimit,
    Network,
    Other,
}

#[cfg(test)]
mod tests {
    use super::*;
    use matches::assert_matches;

    #[test]
    fn test_make_blob_url() {
        let merkle = "00112233445566778899aabbccddeeffffeeddccbbaa99887766554433221100"
            .parse::<BlobId>()
            .unwrap();

        assert_eq!(
            make_blob_url("http://example.com", &merkle).unwrap(),
            format!("http://example.com/{}", merkle).parse::<Uri>().unwrap()
        );

        assert_eq!(
            make_blob_url("http://example.com/noslash", &merkle).unwrap(),
            format!("http://example.com/noslash/{}", merkle).parse::<Uri>().unwrap()
        );

        assert_eq!(
            make_blob_url("http://example.com/slash/", &merkle).unwrap(),
            format!("http://example.com/slash/{}", merkle).parse::<Uri>().unwrap()
        );

        assert_eq!(
            make_blob_url("http://example.com/twoslashes//", &merkle).unwrap(),
            format!("http://example.com/twoslashes//{}", merkle).parse::<Uri>().unwrap()
        );

        assert_matches!(
            make_blob_url("HelloWorld", &merkle).unwrap_err(),
            BlobUrlError::UriWithoutPath
        );

        assert_matches!(
            make_blob_url("server:80", &merkle).unwrap_err(),
            BlobUrlError::UriWithoutPath
        );

        // IPv6 zone id
        assert_eq!(
            make_blob_url("http://[fe80::e022:d4ff:fe13:8ec3%252]:8083/blobs/", &merkle).unwrap(),
            format!("http://[fe80::e022:d4ff:fe13:8ec3%252]:8083/blobs/{}", merkle)
                .parse::<Uri>()
                .unwrap()
        );
    }
}
