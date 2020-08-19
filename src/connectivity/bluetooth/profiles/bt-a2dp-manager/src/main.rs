// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![recursion_limit = "256"]

use {
    anyhow::{anyhow, Error},
    async_utils::event::Event,
    fidl_fuchsia_bluetooth_a2dp::{AudioModeRequest, AudioModeRequestStream, Role},
    fidl_fuchsia_bluetooth_component::{LifecycleMarker, LifecycleState},
    fidl_fuchsia_sys::LauncherProxy,
    fuchsia_async::{self as fasync, Time, TimeoutExt},
    fuchsia_component::{client, fuchsia_single_component_package_url, server::ServiceFs},
    fuchsia_zircon as zx,
    futures::{
        channel::mpsc,
        future::BoxFuture,
        select,
        sink::SinkExt,
        stream::{FuturesUnordered, StreamExt, TryStreamExt},
        FutureExt,
    },
    log::{error, warn},
};

#[cfg(test)]
mod test_util;

/// The maximum expected length of component termination.
const COMPONENT_TERMINATION_TIMEOUT: zx::Duration = zx::Duration::from_seconds(2);

/// The URLs for components required by the A2DP Sink role.
static SINK_COMPONENT_URLS: &'static [&'static str] =
    &[fuchsia_single_component_package_url!("bt-a2dp-sink")];

/// The URLs for components required by the A2DP Source role.
static SOURCE_COMPONENT_URLS: &'static [&'static str] = &[
    fuchsia_single_component_package_url!("bt-a2dp-source"),
    fuchsia_single_component_package_url!("bt-avrcp-target"),
];

/// Returns a slice of URLs required for `role`.
fn into_urls(role: Role) -> &'static [&'static str] {
    match role {
        Role::Source => SOURCE_COMPONENT_URLS,
        Role::Sink => SINK_COMPONENT_URLS,
    }
}

/// Holds the state needed to launch the A2DP Profile component requested by the
/// AudioMode FIDL protocol.
struct Handler {
    launcher: LauncherProxy,
    active_role: Option<Role>,
    children: Vec<client::App>,
    /// Collection of futures of child component termination or error events.
    child_events: FuturesUnordered<BoxFuture<'static, Result<client::ExitStatus, Error>>>,
    /// Signal indicating that a new future has been added to `child_events`.
    child_added: Event,
}

impl Handler {
    /// Create a new `Handler`.
    pub fn new(launcher: LauncherProxy) -> Self {
        Self {
            launcher,
            active_role: None,
            children: vec![],
            child_events: FuturesUnordered::new(),
            child_added: Event::new(),
        }
    }

    /// Handle a single request to set the a2dp role to `role`.
    async fn handle(&mut self, role: Role) -> Result<(), Error> {
        if self.active_role != Some(role) {
            self.active_role = None;
            self.terminate_active_profiles().await?;
            for url in into_urls(role) {
                self.launch_profile(url.to_string()).await?;
            }
            self.active_role = Some(role);
        }
        Ok(())
    }

    /// Terminate all active profile components.
    /// Returns an error if a profile is running but cannot be terminated.
    async fn terminate_active_profiles(&mut self) -> Result<(), Error> {
        // Move collections of children and child_events to stack locals
        // to ensure they are dropped in the event of an early return.
        let mut children = std::mem::replace(&mut self.children, vec![]);
        let events = std::mem::replace(&mut self.child_events, FuturesUnordered::new());

        // Send kill signal to all children.
        let results: Vec<_> = children.iter_mut().map(|c| c.kill()).collect();
        for result in results {
            result?;
        }

        events
            .map(|r: Result<client::ExitStatus, Error>| {
                r.and_then(|e| {
                    if e.code() != zx::sys::ZX_TASK_RETCODE_SYSCALL_KILL {
                        warn!("Unexpected return code on child component exit: {:?}", e);
                    }
                    Ok(())
                })
            })
            .try_collect::<()>()
            .on_timeout(Time::after(COMPONENT_TERMINATION_TIMEOUT), || {
                Err(anyhow!("timeout waiting for component termination"))
            })
            .await
    }

    /// Launch the component associated with the specified `role` and return its `App`
    /// object on success.
    ///
    /// The component is guaranteed to have reached the component lifecycle `Ready` state
    /// before the `App` is returned.
    async fn launch_profile(&mut self, url: String) -> Result<(), Error> {
        let child = client::launch(&self.launcher, url, None)?;
        let lifecycle = child
            .connect_to_service::<LifecycleMarker>()
            .expect("failed to connect to component lifecycle protocol");
        loop {
            match lifecycle.get_state().await? {
                LifecycleState::Initializing => continue,
                LifecycleState::Ready => break,
            }
        }
        let event_stream = child.controller().take_event_stream();
        self.child_events.push(client::ExitStatus::from_event_stream(event_stream).boxed());
        self.children.push(child);
        self.child_added.signal();
        Ok(())
    }

    /// Watch child components that have been launched for the active profile. If early termination
    /// or an error in the component's controller protocol are encountered, this function returns
    /// that error.
    async fn supervise(&mut self) -> Error {
        loop {
            let result = match self.child_events.next().await {
                Some(Ok(status)) => anyhow!("Managed profile died unexpectedly: {}", status),
                Some(Err(e)) => anyhow!("Watching of managed profile failed: {}", e),
                None => {
                    // There are no children under supervision. Wait until there are before
                    // jumping back to the top of the supervision loop.
                    self.child_added.wait().await;
                    self.child_added = Event::new();
                    continue;
                }
            };
            return result;
        }
    }
}

/// Handle a coalesced stream of `AudioModeRequest`s. All requests are serialized and processed in
/// the order the component receives them. A request to switch roles will not be processed until the
/// previous request is complete.
async fn handle_requests(mut requests: mpsc::Receiver<AudioModeRequest>, launcher: LauncherProxy) {
    let mut handler = Handler::new(launcher);

    loop {
        select! {
            request = requests.next().fuse() => {
                if let Some(AudioModeRequest::SetRole { role, responder }) = request {
                    match handler.handle(role).await {
                        Ok(()) => {
                            // Error can be ignored since there is nothing more to do in the case
                            // of an error.
                            let _ = responder.send();
                        }
                        Err(e) => {
                            error!("Failed to set role {:?}: {}", role, e);
                            responder.control_handle().shutdown_with_epitaph(zx::Status::UNAVAILABLE);
                        }
                    }
                } else {
                    // Requests channel closed
                    break;
                }
            }
            exit_status = handler.supervise().fuse() => {
                error!("Active role {:?} exited unexpectedly: {}", handler.active_role, exit_status);
                break;
            }
        }
    }
}

/// Forward requests from a single request stream to the request handler.
fn handle_client_connection(
    mut sender: mpsc::Sender<AudioModeRequest>,
    mut stream: AudioModeRequestStream,
) {
    fasync::Task::spawn(async move {
        while let Some(request) = stream.next().await {
            match request {
                Ok(request) => sender.send(request).await.expect("send to handler failed"),
                Err(e) => error!("Client connection failed: {}", e),
            }
        }
    })
    .detach();
}

#[fasync::run_singlethreaded]
async fn main() {
    fuchsia_syslog::init_with_tags(&["a2dp-manager"]).expect("Can't init logger");
    let launcher = client::launcher().expect("connect to Launcher service");
    let (sender, receiver) = mpsc::channel(0);

    let mut fs = ServiceFs::new();
    fs.dir("svc").add_fidl_service(move |stream| handle_client_connection(sender.clone(), stream));
    fs.take_and_serve_directory_handle().expect("serve ServiceFS directory");
    let mut drive_service_fs = fs.collect::<()>().fuse();

    select! {
        _ = handle_requests(receiver, launcher).fuse() => {}
        _ = drive_service_fs => {}
    }
}

#[cfg(test)]
mod tests {
    use {
        super::{test_util::*, *},
        fidl::endpoints::RequestStream as _,
        fidl_fuchsia_bluetooth_a2dp::AudioModeMarker,
        fidl_fuchsia_sys::LauncherMarker,
        futures::{pin_mut, StreamExt},
        matches::assert_matches,
        std::task::Poll,
    };

    #[test]
    fn into_urls_returns_expected_number_of_packages() {
        assert_eq!(into_urls(Role::Source).len(), 2);
        assert_eq!(into_urls(Role::Sink).len(), 1);
    }

    #[test]
    fn client_requests_are_forwarded() {
        let mut ex = fasync::Executor::new().unwrap();
        let (sender, mut receiver) = mpsc::channel(0);

        let (proxy_1, stream_1) =
            fidl::endpoints::create_proxy_and_stream::<AudioModeMarker>().unwrap();
        handle_client_connection(sender.clone(), stream_1);
        let _ = proxy_1.set_role(Role::Source);

        let (proxy_2, stream_2) =
            fidl::endpoints::create_proxy_and_stream::<AudioModeMarker>().unwrap();
        handle_client_connection(sender.clone(), stream_2);
        let _ = proxy_2.set_role(Role::Sink);

        let result = ex.run_until_stalled(&mut receiver.next());
        assert_matches!(result, Poll::Ready(Some(AudioModeRequest::SetRole { role: Role::Source, ..})));

        let result = ex.run_until_stalled(&mut receiver.next());
        assert_matches!(result, Poll::Ready(Some(AudioModeRequest::SetRole { role: Role::Sink, ..})));
    }

    #[test]
    fn handle_incoming_request_and_launching_child() {
        let mut ex = fasync::Executor::new().unwrap();
        let (mut sender, receiver) = mpsc::channel::<AudioModeRequest>(0);
        let (mut launcher, launcher_proxy) = mock_launcher();
        let (proxy, mut stream) =
            fidl::endpoints::create_proxy_and_stream::<AudioModeMarker>().unwrap();
        let handler = handle_requests(receiver, launcher_proxy);
        pin_mut!(handler);

        let role = Role::Source;
        let expected_urls = into_urls(role);
        // Construct a request that can be sent into the `handle_requests` function.
        let mut response = proxy.set_role(role);
        let request = unwrap_ready!(ex.run_until_stalled(&mut stream.next())).unwrap().unwrap();
        assert!(ex.run_until_stalled(&mut sender.send(request)).is_pending());

        let _ = ex.run_until_stalled(&mut handler);

        let next_n = launcher.next_n(expected_urls.len());
        pin_mut!(next_n);
        let source_components = unwrap_ready!(ex.run_until_stalled(&mut next_n));
        assert_expected_components!(expected_urls, &source_components);

        // Drive the FIDL request/response to completion.
        assert_matches!(ex.run_until_stalled(&mut response), Poll::Ready(Ok(())));
    }

    #[test]
    fn switching_profiles_terminates_the_old_and_launches_the_new() {
        let mut ex = fasync::Executor::new().unwrap();
        let (mut sender, receiver) = mpsc::channel::<AudioModeRequest>(0);
        let (mut launcher, launcher_proxy) = mock_launcher();
        let (proxy, mut stream) =
            fidl::endpoints::create_proxy_and_stream::<AudioModeMarker>().unwrap();
        let handler = handle_requests(receiver, launcher_proxy);
        pin_mut!(handler);

        // A first request
        let source_components = {
            let role = Role::Source;
            let expected_urls = into_urls(role);
            // Construct a request that can be sent into the `handle_requests` function.
            let mut response = proxy.set_role(role);
            let request = unwrap_ready!(ex.run_until_stalled(&mut stream.next())).unwrap().unwrap();
            assert!(ex.run_until_stalled(&mut sender.send(request)).is_pending());

            let _ = ex.run_until_stalled(&mut handler);

            let next_n = launcher.next_n(expected_urls.len());
            pin_mut!(next_n);
            let source_components = unwrap_ready!(ex.run_until_stalled(&mut next_n));
            assert_expected_components!(expected_urls, &source_components);

            // Drive the FIDL request/response to completion.
            assert_matches!(ex.run_until_stalled(&mut response), Poll::Ready(Ok(())));
            source_components
        };

        // A second request
        {
            let role = Role::Sink;
            let expected_urls = into_urls(role);

            let mut response = proxy.set_role(role);
            let request = unwrap_ready!(ex.run_until_stalled(&mut stream.next())).unwrap().unwrap();
            assert!(ex.run_until_stalled(&mut sender.send(request)).is_pending());

            let _ = ex.run_until_stalled(&mut handler);

            assert!(source_components.iter().all(|c| c.terminated()));

            let next_n = launcher.next_n(expected_urls.len());
            pin_mut!(next_n);
            let sink_components = unwrap_ready!(ex.run_until_stalled(&mut next_n));
            assert_expected_components!(expected_urls, sink_components);

            // Drive the FIDL request/response to completion.
            assert_matches!(ex.run_until_stalled(&mut response), Poll::Ready(Ok(())));
        }
    }

    #[test]
    fn sending_the_same_request_twice_does_nothing() {
        let mut ex = fasync::Executor::new().unwrap();
        let (mut sender, receiver) = mpsc::channel::<AudioModeRequest>(0);
        let (mut launcher, launcher_proxy) = mock_launcher();
        let (proxy, mut stream) =
            fidl::endpoints::create_proxy_and_stream::<AudioModeMarker>().unwrap();
        let handler = handle_requests(receiver, launcher_proxy);
        pin_mut!(handler);

        // A first request
        let source_components = {
            let role = Role::Source;
            let expected_urls = into_urls(role);
            // Construct a request that can be sent into the `handle_requests` function.
            let mut response = proxy.set_role(role);
            let request = unwrap_ready!(ex.run_until_stalled(&mut stream.next())).unwrap().unwrap();
            assert!(ex.run_until_stalled(&mut sender.send(request)).is_pending());

            let _ = ex.run_until_stalled(&mut handler);

            let next_n = launcher.next_n(expected_urls.len());
            pin_mut!(next_n);
            let source_components = unwrap_ready!(ex.run_until_stalled(&mut next_n));
            assert_expected_components!(expected_urls, &source_components);

            // Drive the FIDL request/response to completion.
            assert_matches!(ex.run_until_stalled(&mut response), Poll::Ready(Ok(())));
            source_components
        };

        // A second request for the same role
        {
            let role = Role::Source;

            let _ = proxy.set_role(role);
            let request = unwrap_ready!(ex.run_until_stalled(&mut stream.next())).unwrap().unwrap();
            assert!(ex.run_until_stalled(&mut sender.send(request)).is_pending());

            let _ = ex.run_until_stalled(&mut handler);

            assert!(source_components.iter().all(|c| !c.terminated()));

            // No new components have been launched.
            let next_n = launcher.next_n(1);
            pin_mut!(next_n);
            assert!(ex.run_until_stalled(&mut next_n).is_pending());
        }
    }

    #[test]
    fn handler_completes_on_unexpected_child_termination() {
        let mut ex = fasync::Executor::new().unwrap();
        let (mut sender, receiver) = mpsc::channel::<AudioModeRequest>(0);
        let (mut launcher, launcher_proxy) = mock_launcher();
        let (proxy, mut stream) =
            fidl::endpoints::create_proxy_and_stream::<AudioModeMarker>().unwrap();
        let handler = handle_requests(receiver, launcher_proxy);
        pin_mut!(handler);

        let role = Role::Source;
        let expected_urls = into_urls(role);
        // Construct a request that can be sent into the `handle_requests` function.
        let mut response = proxy.set_role(role);
        let request = unwrap_ready!(ex.run_until_stalled(&mut stream.next())).unwrap().unwrap();
        assert!(ex.run_until_stalled(&mut sender.send(request)).is_pending());

        let _ = ex.run_until_stalled(&mut handler);

        let next_n = launcher.next_n(expected_urls.len());
        pin_mut!(next_n);
        let source_components = unwrap_ready!(ex.run_until_stalled(&mut next_n));
        assert_expected_components!(expected_urls, &source_components);

        // Drive the FIDL request/response to completion.
        assert_matches!(ex.run_until_stalled(&mut response), Poll::Ready(Ok(())));

        // Handler is in a steady state.
        assert!(ex.run_until_stalled(&mut handler).is_pending());

        // Terminating a child component causes handler to complete
        source_components[0].terminate();
        assert!(ex.run_until_stalled(&mut handler).is_ready());
    }

    #[test]
    fn handler_completes_on_disconnected_launcher_proxy() {
        let mut ex = fasync::Executor::new().unwrap();
        let (mut sender, receiver) = mpsc::channel::<AudioModeRequest>(0);
        let (launcher_proxy, launcher_stream) =
            fidl::endpoints::create_proxy_and_stream::<LauncherMarker>().unwrap();
        launcher_stream.control_handle().shutdown();
        drop(launcher_stream);

        let (proxy, mut stream) =
            fidl::endpoints::create_proxy_and_stream::<AudioModeMarker>().unwrap();
        let handler = handle_requests(receiver, launcher_proxy);
        pin_mut!(handler);

        let role = Role::Source;
        // Construct a request that can be sent into the `handle_requests` function.
        let mut response = proxy.set_role(role);
        let request = unwrap_ready!(ex.run_until_stalled(&mut stream.next())).unwrap().unwrap();
        assert!(ex.run_until_stalled(&mut sender.send(request)).is_pending());

        // Handler should process some work and return pending indicating that it can continue
        // processing.
        assert_matches!(ex.run_until_stalled(&mut handler), Poll::Pending);

        // The response is an error because the launcher_proxy in use by the handler could not
        // launch the component.
        assert_matches!(ex.run_until_stalled(&mut response), Poll::Ready(Err(_)));
    }
}
