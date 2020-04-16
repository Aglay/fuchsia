// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{clock, inspect_util},
    fidl_fuchsia_pkg_ext::MirrorConfig,
    fuchsia_async as fasync,
    fuchsia_hyper::HyperConnector,
    fuchsia_inspect::{self as inspect, Property},
    fuchsia_inspect_contrib::inspectable::InspectableDebugString,
    fuchsia_syslog::{fx_log_err, fx_log_info},
    fuchsia_zircon as zx,
    futures::{
        future::{AbortHandle, Abortable, FutureExt},
        lock::Mutex as AsyncMutex,
        stream::StreamExt,
    },
    http_uri_ext::HttpUriExt as _,
    hyper_rustls::HttpsConnector,
    std::sync::{Arc, Weak},
    tuf::{
        error::Error as TufError,
        interchange::Json,
        metadata::{TargetDescription, TargetPath},
        repository::{HttpRepository, RepositoryProvider, RepositoryStorage},
    },
};

pub struct UpdatingTufClient<L>
where
    L: RepositoryStorage<Json> + RepositoryProvider<Json>,
{
    client: tuf::client::Client<
        Json,
        L,
        HttpRepository<HttpsConnector<HyperConnector>, Json>,
        tuf::client::DefaultTranslator,
    >,

    /// Time that this repository was last successfully checked for an update, or None if the
    /// repository has never successfully fetched target metadata.
    last_update_successfully_checked_time: InspectableDebugString<Option<zx::Time>>,

    /// `Some` if there is an AutoClient task, dropping it stops the task.
    auto_client_aborter: Option<AbortHandleOnDrop>,

    inspect: UpdatingTufClientInspectState,
}

struct AbortHandleOnDrop {
    abort_handle: AbortHandle,
}

impl Drop for AbortHandleOnDrop {
    fn drop(&mut self) {
        self.abort_handle.abort();
    }
}

impl From<AbortHandle> for AbortHandleOnDrop {
    fn from(abort_handle: AbortHandle) -> Self {
        Self { abort_handle }
    }
}

struct UpdatingTufClientInspectState {
    /// Count of the number of times this repository failed to check for an update.
    update_check_failure_count: inspect_util::Counter,

    /// Count of the number of times this repository was successfully checked for an update.
    update_check_success_count: inspect_util::Counter,

    /// Count of the number of times this repository was successfully updated.
    updated_count: inspect_util::Counter,

    /// Version of the active timestamp file, or 0 if unknown.
    timestamp_version: inspect::UintProperty,

    /// Version of the active snapshot file, or 0 if unknown.
    snapshot_version: inspect::UintProperty,

    /// Version of the active targets file, or 0 if unknown.
    targets_version: inspect::UintProperty,

    _node: inspect::Node,
}

/// Result of updating metadata if stale.
pub enum UpdateResult {
    /// Metadata update was skipped because it was not stale.
    Deferred,

    /// Local metadata is up to date with the remote.
    UpToDate,

    /// Some metadata was updated.
    Updated,
}

impl<L> UpdatingTufClient<L>
where
    L: RepositoryStorage<Json> + RepositoryProvider<Json> + 'static,
{
    pub fn from_tuf_client_and_mirror_config(
        client: tuf::client::Client<
            Json,
            L,
            HttpRepository<HttpsConnector<HyperConnector>, Json>,
            tuf::client::DefaultTranslator,
        >,
        config: &MirrorConfig,
        node: inspect::Node,
    ) -> Arc<AsyncMutex<Self>> {
        let (auto_client_aborter, auto_client_node_and_registration) = if config.subscribe() {
            let (aborter, registration) = AbortHandle::new_pair();
            let auto_client_node = node.create_child("auto_client");
            (Some(aborter.into()), Some((auto_client_node, registration)))
        } else {
            (None, None)
        };
        let ret = Arc::new(AsyncMutex::new(Self {
            client,
            last_update_successfully_checked_time: InspectableDebugString::new(
                None,
                &node,
                "last_update_successfully_checked_time",
            ),
            auto_client_aborter,
            inspect: UpdatingTufClientInspectState {
                update_check_failure_count: inspect_util::Counter::new(
                    &node,
                    "update_check_failure_count",
                ),
                update_check_success_count: inspect_util::Counter::new(
                    &node,
                    "update_check_success_count",
                ),
                updated_count: inspect_util::Counter::new(&node, "updated_count"),
                timestamp_version: node.create_uint("timestamp_version", 0),
                snapshot_version: node.create_uint("snapshot_version", 0),
                targets_version: node.create_uint("targets_version", 0),
                _node: node,
            },
        }));

        if let Some((node, registration)) = auto_client_node_and_registration {
            fasync::spawn_local(
                Abortable::new(
                    AutoClient::from_updating_client_and_auto_url(
                        Arc::downgrade(&ret),
                        config
                            .mirror_url()
                            .to_owned()
                            .extend_dir_with_path("auto")
                            // Safe because mirror_url has a scheme and "auto" is a valid path segment.
                            .unwrap()
                            .to_string(),
                        node,
                    )
                    .run(),
                    registration,
                )
                .map(|_| ()),
            );
        }

        ret
    }

    pub async fn fetch_target_description(
        &mut self,
        target: &TargetPath,
    ) -> Result<TargetDescription, TufError> {
        self.client.fetch_target_description(target).await
    }

    /// Updates the tuf client metadata if it is considered to be stale, returning whether or not
    /// updates were performed.
    pub async fn update_if_stale(&mut self) -> Result<UpdateResult, TufError> {
        if self.is_stale() {
            if self.update().await? {
                Ok(UpdateResult::Updated)
            } else {
                Ok(UpdateResult::UpToDate)
            }
        } else {
            Ok(UpdateResult::Deferred)
        }
    }

    /// Provides the current known metadata versions (timestamp, snapshot, targets).
    pub fn metadata_versions(&self) -> (Option<u32>, Option<u32>, Option<u32>) {
        (
            self.client.timestamp_version(),
            self.client.snapshot_version(),
            self.client.targets_version(),
        )
    }

    fn is_stale(&self) -> bool {
        if self.auto_client_aborter.is_none() {
            return true;
        }
        if let Some(last_update_time) = *self.last_update_successfully_checked_time {
            last_update_time + SUBSCRIBE_CACHE_STALE_TIMEOUT <= clock::now()
        } else {
            true
        }
    }

    async fn update(&mut self) -> Result<bool, TufError> {
        let res = self.client.update().await;
        self.inspect.timestamp_version.set(self.client.timestamp_version().unwrap_or(0).into());
        self.inspect.snapshot_version.set(self.client.snapshot_version().unwrap_or(0).into());
        self.inspect.targets_version.set(self.client.targets_version().unwrap_or(0).into());
        if let Ok(update_occurred) = &res {
            self.last_update_successfully_checked_time.get_mut().replace(clock::now());
            self.inspect.update_check_success_count.increment();
            if *update_occurred {
                self.inspect.updated_count.increment();
            }
        } else {
            self.inspect.update_check_failure_count.increment();
        }
        res
    }
}

pub const SUBSCRIBE_CACHE_STALE_TIMEOUT: zx::Duration = zx::Duration::from_minutes(5);

struct AutoClient<L>
where
    L: RepositoryStorage<Json> + RepositoryProvider<Json>,
{
    updating_client: Weak<AsyncMutex<UpdatingTufClient<L>>>,
    auto_url: String,
    inspect: AutoClientInspectState,
}

struct AutoClientInspectState {
    connect_failure_count: inspect_util::Counter,
    connect_success_count: inspect_util::Counter,
    update_attempt_count: inspect_util::Counter,
    _node: inspect::Node,
}

#[cfg(not(test))]
const AUTO_CLIENT_SSE_RECONNECT_DELAY: zx::Duration = zx::Duration::from_minutes(1);

#[cfg(test)]
const AUTO_CLIENT_SSE_RECONNECT_DELAY: zx::Duration = zx::Duration::from_minutes(0);

impl<L> AutoClient<L>
where
    L: RepositoryStorage<Json> + RepositoryProvider<Json> + 'static,
{
    fn from_updating_client_and_auto_url(
        updating_client: Weak<AsyncMutex<UpdatingTufClient<L>>>,
        auto_url: String,
        node: inspect::Node,
    ) -> Self {
        Self {
            updating_client,
            auto_url,
            inspect: AutoClientInspectState {
                connect_failure_count: inspect_util::Counter::new(&node, "connect_failure_count"),
                connect_success_count: inspect_util::Counter::new(&node, "connect_success_count"),
                update_attempt_count: inspect_util::Counter::new(&node, "update_attempt_count"),
                _node: node,
            },
        }
    }

    async fn run(self) {
        loop {
            fx_log_info!("AutoClient for {:?} connecting.", self.auto_url);
            match self.connect().await {
                Ok(sse_client) => match self.handle_sse(sse_client).await {
                    HandleSseEndState::Abort => {
                        return;
                    }
                    HandleSseEndState::Reconnect => (),
                },
                Err(e) => {
                    fx_log_err!("AutoClient for {:?} error connecting: {:?}", self.auto_url, e);
                }
            }
            Self::wait_before_reconnecting().await;
        }
    }

    async fn connect(&self) -> Result<http_sse::Client, http_sse::ClientConnectError> {
        // The /auto protocol has no heartbeat, so, without TCP keepalive, a client cannot
        // differentiate a repository that is not updating from a repository that has dropped
        // the connection.
        let mut tcp_options = fuchsia_hyper::TcpOptions::default();
        tcp_options.keepalive_idle = Some(std::time::Duration::from_secs(15));
        tcp_options.keepalive_interval = Some(std::time::Duration::from_secs(5));
        tcp_options.keepalive_count = Some(2);
        match http_sse::Client::connect(
            fuchsia_hyper::new_https_client_from_tcp_options(tcp_options),
            &self.auto_url,
        )
        .await
        {
            Ok(sse_client) => {
                self.inspect.connect_success_count.increment();
                Ok(sse_client)
            }
            Err(e) => {
                self.inspect.connect_failure_count.increment();
                Err(e)
            }
        }
    }

    async fn handle_sse(&self, mut sse_event_stream: http_sse::Client) -> HandleSseEndState {
        while let Some(item) = sse_event_stream.next().await {
            match item {
                Ok(event) => {
                    if event.event_type() != "timestamp.json" {
                        fx_log_err!(
                            "AutoClient for {:?} ignoring unrecognized event: {:?}",
                            self.auto_url,
                            event
                        );
                        continue;
                    }
                    fx_log_info!(
                        "AutoClient for {:?} observed valid event: {:?}",
                        self.auto_url,
                        event
                    );
                    if let Some(updating_client) = self.updating_client.upgrade() {
                        self.inspect.update_attempt_count.increment();
                        if let Err(e) = updating_client.lock().await.update().await {
                            fx_log_err!(
                                "AutoClient for {:?} error updating TUF client: {:?}",
                                self.auto_url,
                                e
                            );
                        }
                    } else {
                        return HandleSseEndState::Abort;
                    }
                }
                Err(e) => {
                    fx_log_err!(
                        "AutoClient for {:?} event stream read error: {:?}",
                        self.auto_url,
                        e
                    );
                    return HandleSseEndState::Reconnect;
                }
            }
        }
        fx_log_err!("AutoClient for {:?} event stream closed.", self.auto_url);
        HandleSseEndState::Reconnect
    }

    async fn wait_before_reconnecting() {
        fasync::Timer::new(fasync::Time::after(AUTO_CLIENT_SSE_RECONNECT_DELAY)).await
    }
}

impl<L> Drop for AutoClient<L>
where
    L: RepositoryStorage<Json> + RepositoryProvider<Json>,
{
    fn drop(&mut self) {
        fx_log_info!("AutoClient for {:?} stopping.", self.auto_url);
    }
}

enum HandleSseEndState {
    Reconnect,
    Abort,
}
