// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{format_err, Context, Error},
    fidl_fuchsia_diagnostics::{
        ArchiveAccessorMarker, ArchiveAccessorProxy, BatchIteratorMarker,
        ClientSelectorConfiguration, DataType, Format, FormattedContent, SelectorArgument,
        StreamMode, StreamParameters,
    },
    fuchsia_async::{self as fasync, DurationExt, TimeoutExt},
    fuchsia_component::client,
    fuchsia_inspect_node_hierarchy::{
        serialization::{HierarchyDeserializer, RawJsonNodeHierarchySerializer},
        NodeHierarchy,
    },
    fuchsia_zircon::{Duration, DurationNum},
    lazy_static::lazy_static,
    serde_json,
};

lazy_static! {
    static ref RETRY_DELAY_MS: i64 = 150;
    static ref MAX_RETRIES: usize = std::usize::MAX;
    static ref PAYLOAD_KEY: &'static str = "payload";
}

/// An inspect tree selector for a component.
pub struct ComponentSelector {
    relative_moniker: Vec<String>,
    tree_selectors: Vec<String>,
}

impl ComponentSelector {
    /// Create a new component event selector.
    /// By default it will select the whole tree unless tree selectors are provided.
    /// `relative_moniker` is the realm path relative to the realm of the running component plus the
    /// component name. For example: [a, b, component.cmx].
    pub fn new(relative_moniker: Vec<String>) -> Self {
        Self { relative_moniker, tree_selectors: Vec::new() }
    }

    /// Select a section of the inspect tree.
    pub fn with_tree_selector(mut self, tree_selector: impl Into<String>) -> Self {
        self.tree_selectors.push(tree_selector.into());
        self
    }

    fn relative_moniker_str(&self) -> String {
        self.relative_moniker.join("/")
    }
}

pub trait ToSelectorArguments {
    fn to_selector_arguments(self) -> Vec<String>;
}

impl ToSelectorArguments for String {
    fn to_selector_arguments(self) -> Vec<String> {
        vec![self]
    }
}

impl ToSelectorArguments for &str {
    fn to_selector_arguments(self) -> Vec<String> {
        vec![self.to_string()]
    }
}

impl ToSelectorArguments for ComponentSelector {
    fn to_selector_arguments(self) -> Vec<String> {
        let relative_moniker = self.relative_moniker_str();
        // If not tree selectors were provided, select the full tree.
        if self.tree_selectors.is_empty() {
            vec![format!("{}:root", relative_moniker.clone())]
        } else {
            self.tree_selectors
                .iter()
                .map(|s| format!("{}:{}", relative_moniker.clone(), s.clone()))
                .collect()
        }
    }
}

/// Utility for reading inspect data of a running component using the injected observer.cmx Archive
/// Reader service.
pub struct InspectDataFetcher {
    archive: Option<ArchiveAccessorProxy>,
    selectors: Vec<String>,
    should_retry: bool,
    timeout: Option<Duration>,
}

impl InspectDataFetcher {
    /// Creates a new data fetcher with default configuration:
    ///  - Maximum retries: 2^64-1
    ///  - Timeout: Never. Use with_timeout() to set a timeout.
    pub fn new() -> Self {
        Self { timeout: None, selectors: vec![], should_retry: true, archive: None }
    }

    pub fn with_archive(mut self, archive: ArchiveAccessorProxy) -> Self {
        self.archive = Some(archive);
        self
    }

    /// Requests a single component tree (or sub-tree).
    pub fn add_selector(mut self, selector: impl ToSelectorArguments) -> Self {
        self.selectors.extend(selector.to_selector_arguments().into_iter());
        self
    }

    /// Requests to retry when an empty result is received.
    pub fn retry_if_empty(mut self, retry: bool) -> Self {
        self.should_retry = retry;
        self
    }

    pub fn add_selectors<T, S>(self, selectors: T) -> Self
    where
        T: Iterator<Item = S>,
        S: ToSelectorArguments,
    {
        let mut this = self;
        for selector in selectors {
            this = this.add_selector(selector);
        }
        this
    }

    /// Sets the maximum time to wait for a response from the Archive.
    /// Do not use in tests unless timeout is the expected behavior.
    pub fn with_timeout(mut self, duration: Duration) -> Self {
        self.timeout = Some(duration);
        self
    }

    /// Connects to the archivist observer.cmx and returns inspect data associated with the given
    /// component under the relative realm path given.
    pub async fn get(self) -> Result<Vec<NodeHierarchy>, Error> {
        let raw_json = self.get_raw_json().await?;
        match raw_json {
            serde_json::Value::Array(values) => values
                .into_iter()
                .map(|mut value| {
                    let tree_json =
                        value.get_mut(*PAYLOAD_KEY).context("contents are there")?.take();
                    RawJsonNodeHierarchySerializer::deserialize(tree_json)
                })
                .collect::<Result<Vec<_>, _>>(),
            _ => unreachable!("No other json value type is expected here"),
        }
    }

    /// Connects to the archivist observer.cmx and returns inspect data associated with the given
    /// component under the relative realm path given. Returns the raw json for each hierarchy
    /// fetched.
    pub async fn get_raw_json(self) -> Result<serde_json::Value, Error> {
        let timeout = self.timeout;
        let data_future = self.get_inspect_data();
        let data = match timeout {
            Some(timeout) => data_future.on_timeout(timeout.after_now(), || Ok(Vec::new())).await?,
            None => data_future.await?,
        };
        Ok(serde_json::Value::Array(data))
    }

    async fn get_inspect_data(self) -> Result<Vec<serde_json::Value>, Error> {
        let archive = self.archive.unwrap_or(
            client::connect_to_service::<ArchiveAccessorMarker>().context("connect to archive")?,
        );

        let mut retry = 0;

        loop {
            if retry > *MAX_RETRIES {
                return Err(format_err!("Maximum retries"));
            }

            let (iterator, server_end) = fidl::endpoints::create_proxy::<BatchIteratorMarker>()
                .context("failed to create iterator proxy")?;

            let mut stream_parameters = StreamParameters::empty();
            stream_parameters.stream_mode = Some(StreamMode::Snapshot);
            stream_parameters.data_type = Some(DataType::Inspect);
            stream_parameters.format = Some(Format::Json);
            stream_parameters.client_selector_configuration = if self.selectors.is_empty() {
                Some(ClientSelectorConfiguration::SelectAll(true))
            } else {
                Some(ClientSelectorConfiguration::Selectors(
                    self.selectors
                        .iter()
                        .map(|selector| SelectorArgument::RawSelector(selector.clone()))
                        .collect(),
                ))
            };

            archive
                .stream_diagnostics(stream_parameters, server_end)
                .context("get BatchIterator")
                .unwrap();

            let mut result = Vec::new();
            loop {
                let next_batch = iterator.get_next().await.context("failed to get batch")?.unwrap();
                if next_batch.is_empty() {
                    break;
                }
                for formatted_content in next_batch {
                    match formatted_content {
                        FormattedContent::Json(data) => {
                            let mut buf = vec![0; data.size as usize];
                            data.vmo.read(&mut buf, 0).context("reading vmo")?;
                            let hierarchy_json = std::str::from_utf8(&buf).unwrap();
                            let output: serde_json::Value =
                                serde_json::from_str(&hierarchy_json).context("valid json")?;
                            result.push(output);
                        }
                        _ => unreachable!(
                            "JSON was requested, no other data type should be received"
                        ),
                    }
                }
            }

            if result.is_empty() && self.should_retry {
                // Retry with delay to ensure data appears if the reader is called right after the
                // component started, before the archivist knows about it.
                fasync::Timer::new(fasync::Time::after(RETRY_DELAY_MS.millis())).await;
            } else {
                return Ok(result);
            }

            retry += 1;
        }
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        anyhow::format_err,
        fidl_fuchsia_diagnostics as fdiagnostics,
        fidl_fuchsia_sys::ComponentControllerEvent,
        fuchsia_component::{
            client::App,
            server::{NestedEnvironment, ServiceFs},
        },
        fuchsia_zircon as zx,
        futures::StreamExt,
        futures::TryStreamExt,
    };

    const TEST_COMPONENT_URL: &str =
        "fuchsia-pkg://fuchsia.com/fuchsia_inspect_tests#meta/inspect_test_component.cmx";

    async fn start_component(env_label: &str) -> Result<(NestedEnvironment, App), Error> {
        let mut service_fs = ServiceFs::new();
        let env = service_fs.create_nested_environment(env_label)?;
        let app = client::launch(&env.launcher(), TEST_COMPONENT_URL.to_string(), None)?;
        fasync::spawn(service_fs.collect());
        let mut component_stream = app.controller().take_event_stream();
        match component_stream
            .next()
            .await
            .expect("component event stream ended before termination event")?
        {
            ComponentControllerEvent::OnTerminated { return_code, termination_reason } => {
                return Err(format_err!(
                    "Component terminated unexpectedly. Code: {}. Reason: {:?}",
                    return_code,
                    termination_reason
                ));
            }
            ComponentControllerEvent::OnDirectoryReady {} => {}
        }
        Ok((env, app))
    }

    #[fasync::run_singlethreaded(test)]
    async fn inspect_data_for_component() -> Result<(), Error> {
        let (_env, _app) = start_component("test-ok").await?;

        let hierarchies = InspectDataFetcher::new()
            .add_selector("test-ok/inspect_test_component.cmx:root".to_string())
            .get()
            .await?;

        assert_eq!(hierarchies.len(), 1);
        assert_inspect_tree!(hierarchies[0], root: {
            int: 3i64,
            "lazy-node": {
                a: "test",
                child: {
                    double: 3.14,
                },
            }
        });

        let mut response = InspectDataFetcher::new()
            .add_selector(
                ComponentSelector::new(vec![
                    "test-ok".to_string(),
                    "inspect_test_component.cmx".to_string(),
                ])
                .with_tree_selector("root:int")
                .with_tree_selector("root/lazy-node:a"),
            )
            .get_raw_json()
            .await?;

        let hierarchies = response.as_array_mut().expect("as array ok");
        assert_eq!(hierarchies.len(), 1);
        let hierarchy =
            RawJsonNodeHierarchySerializer::deserialize(hierarchies[0][*PAYLOAD_KEY].take())
                .expect("deserialize ok");

        assert_inspect_tree!(hierarchy, root: {
            int: 3i64,
            "lazy-node": {
                a: "test"
            }
        });

        Ok(())
    }

    #[fasync::run_singlethreaded(test)]
    async fn timeout() -> Result<(), Error> {
        let (_env, _app) = start_component("test-timeout").await?;

        let result = InspectDataFetcher::new()
            .add_selector("test-timeout/inspect_test_component.cmx:root")
            .with_timeout(0.nanos())
            .get()
            .await;
        assert!(result.unwrap().is_empty());
        Ok(())
    }

    #[fasync::run_singlethreaded(test)]
    async fn component_selector() {
        let selector = ComponentSelector::new(vec!["a.cmx".to_string()]);
        assert_eq!(selector.relative_moniker_str(), "a.cmx");
        let arguments: Vec<String> = selector.to_selector_arguments();
        assert_eq!(arguments, vec!["a.cmx:root".to_string()]);

        let selector =
            ComponentSelector::new(vec!["b".to_string(), "c".to_string(), "a.cmx".to_string()]);
        assert_eq!(selector.relative_moniker_str(), "b/c/a.cmx");

        let selector = selector.with_tree_selector("root/b/c:d").with_tree_selector("root/e:f");
        let arguments: Vec<String> = selector.to_selector_arguments();
        assert_eq!(
            arguments,
            vec!["b/c/a.cmx:root/b/c:d".to_string(), "b/c/a.cmx:root/e:f".to_string(),]
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn custom_archive() {
        let proxy = spawn_fake_archive();
        let result = InspectDataFetcher::new().with_archive(proxy).get().await.expect("got result");
        assert_eq!(result.len(), 1);
        assert_inspect_tree!(result[0], root: { x: 1i64 });
    }

    fn spawn_fake_archive() -> fdiagnostics::ArchiveAccessorProxy {
        let (proxy, mut stream) =
            fidl::endpoints::create_proxy_and_stream::<fdiagnostics::ArchiveAccessorMarker>()
                .expect("create proxy");
        fasync::spawn(async move {
            while let Some(request) = stream.try_next().await.expect("stream request") {
                match request {
                    fdiagnostics::ArchiveAccessorRequest::StreamDiagnostics {
                        result_stream,
                        ..
                    } => {
                        fasync::spawn(async move {
                            let mut called = false;
                            let mut stream = result_stream.into_stream().expect("into stream");
                            while let Some(req) = stream.try_next().await.expect("stream request") {
                                match req {
                                    fdiagnostics::BatchIteratorRequest::GetNext { responder } => {
                                        if called {
                                            responder
                                                .send(&mut Ok(Vec::new()))
                                                .expect("send response");
                                            continue;
                                        }
                                        called = true;
                                        let content =
                                            serde_json::to_string_pretty(&serde_json::json!({
                                                "moniker": "a",
                                                "payload": {
                                                    "root": {
                                                        "x": 1,
                                                    }
                                                }
                                            }))
                                            .expect("json pretty");
                                        let vmo_size = content.len() as u64;
                                        let vmo =
                                            zx::Vmo::create(vmo_size as u64).expect("create vmo");
                                        vmo.write(content.as_bytes(), 0).expect("write vmo");
                                        let buffer =
                                            fidl_fuchsia_mem::Buffer { vmo, size: vmo_size };
                                        responder
                                            .send(&mut Ok(vec![
                                                fdiagnostics::FormattedContent::Json(buffer),
                                            ]))
                                            .expect("send response");
                                    }
                                }
                            }
                        });
                    }
                }
            }
        });
        return proxy;
    }
}
