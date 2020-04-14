// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::constants::{LOCAL_SOCAT, MAX_RETRY_COUNT, RETRY_DELAY, SOCAT, SOCKET, TARGET_SOCAT},
    crate::discovery::{TargetFinder, TargetFinderConfig},
    crate::mdns::MdnsTargetFinder,
    crate::target::{RCSConnection, Target, TargetCollection},
    anyhow::{anyhow, Context, Error},
    ascendd_lib::run_ascendd,
    async_std::task,
    async_trait::async_trait,
    fidl::endpoints::{ClientEnd, RequestStream, ServiceMarker},
    fidl_fidl_developer_bridge::{DaemonMarker, DaemonRequest, DaemonRequestStream},
    fidl_fuchsia_developer_remotecontrol::{ComponentControlError, RemoteControlMarker},
    fidl_fuchsia_overnet::{
        ServiceConsumerProxyInterface, ServiceProviderRequest, ServiceProviderRequestStream,
    },
    fidl_fuchsia_test_manager as ftest_manager,
    futures::channel::mpsc,
    futures::lock::Mutex,
    futures::prelude::*,
    hoist::spawn,
    std::process::Command,
    std::rc::Rc,
    std::sync::Arc,
    std::time::Duration,
};

mod constants;
mod discovery;
mod mdns;
mod net;
mod target;
mod util;

async fn start_ascendd() {
    log::info!("Starting ascendd");
    spawn(async move {
        run_ascendd(ascendd_lib::Opt { sockpath: Some(SOCKET.to_string()), ..Default::default() })
            .await
            .unwrap();
    });
    log::info!("Connecting to target");
    Command::new(SOCAT).arg(LOCAL_SOCAT).arg(TARGET_SOCAT).spawn().unwrap();
}

/// A locked TargetCollection that has been acquired via the Mutex::lock fn.
pub type GuardedTargetCollection = Arc<Mutex<TargetCollection>>;

#[async_trait]
pub trait DiscoveryHook {
    async fn on_new_target(&self, target: &Arc<Target>, tc: &GuardedTargetCollection);
}

#[derive(Default)]
struct RCSActivatorHook {}

#[async_trait]
impl DiscoveryHook for RCSActivatorHook {
    async fn on_new_target(&self, target: &Arc<Target>, _tc: &GuardedTargetCollection) {
        let mut state = target.state.lock().await;
        if state.overnet_started {
            return;
        }
        match Daemon::start_remote_control(&target.nodename).await {
            Ok(()) => state.overnet_started = true,
            Err(e) => {
                log::warn!("unable to start remote control for '{}': {}", target.nodename, e);
                return;
            }
        }
    }
}

// Daemon
#[derive(Clone)]
pub struct Daemon {
    target_collection: Arc<Mutex<TargetCollection>>,

    discovered_target_hooks: Arc<Mutex<Vec<Rc<dyn DiscoveryHook>>>>,
}

impl Daemon {
    pub async fn new() -> Result<Daemon, Error> {
        let (tx, rx) = mpsc::unbounded::<Target>();
        let target_collection = Arc::new(Mutex::new(TargetCollection::new()));
        let discovered_target_hooks = Arc::new(Mutex::new(Vec::<Rc<dyn DiscoveryHook>>::new()));
        Daemon::spawn_receiver_loop(
            rx,
            Arc::clone(&target_collection),
            Arc::clone(&discovered_target_hooks),
        );
        Daemon::spawn_onet_discovery(Arc::clone(&target_collection));
        let mut d = Daemon {
            target_collection: Arc::clone(&target_collection),
            discovered_target_hooks: Arc::clone(&discovered_target_hooks),
        };
        d.register_hook(RCSActivatorHook::default()).await;

        // MDNS must be started as late as possible to avoid races with registered
        // hooks.
        let config =
            TargetFinderConfig { broadcast_interval: Duration::from_secs(120), mdns_ttl: 255 };
        let mdns = MdnsTargetFinder::new(&config)?;
        mdns.start(&tx)?;

        Ok(d)
    }

    pub async fn register_hook(&mut self, cb: impl DiscoveryHook + 'static) {
        let mut hooks = self.discovered_target_hooks.lock().await;
        hooks.push(Rc::new(cb));
    }

    pub fn spawn_receiver_loop(
        mut rx: mpsc::UnboundedReceiver<Target>,
        tc: Arc<Mutex<TargetCollection>>,
        hooks: Arc<Mutex<Vec<Rc<dyn DiscoveryHook>>>>,
    ) {
        spawn(async move {
            loop {
                let target = rx.next().await.unwrap();
                let nodename = target.nodename.clone();
                let mut tc_mut = tc.lock().await;
                tc_mut.merge_insert(target).await;
                let target_clone = Arc::clone(tc_mut.get(nodename.into()).await.unwrap());
                let tc_clone = Arc::clone(&tc);
                let hooks_clone = (*hooks.lock().await).clone();
                spawn(async move {
                    futures::future::join_all(
                        hooks_clone.iter().map(|hook| hook.on_new_target(&target_clone, &tc_clone)),
                    )
                    .await;
                });
            }
        });
    }

    pub fn new_with_rx(rx: mpsc::UnboundedReceiver<Target>) -> Daemon {
        let target_collection = Arc::new(Mutex::new(TargetCollection::new()));
        let discovered_target_hooks = Arc::new(Mutex::new(Vec::<Rc<dyn DiscoveryHook>>::new()));
        Daemon::spawn_receiver_loop(
            rx,
            Arc::clone(&target_collection),
            Arc::clone(&discovered_target_hooks),
        );
        Daemon { target_collection, discovered_target_hooks }
    }

    pub async fn handle_requests_from_stream(
        &self,
        mut stream: DaemonRequestStream,
        quiet: bool,
    ) -> Result<(), Error> {
        while let Some(req) = stream.try_next().await? {
            self.handle_request(req, quiet).await?;
        }
        Ok(())
    }

    pub fn spawn_onet_discovery(tc: Arc<Mutex<TargetCollection>>) {
        spawn(async move {
            let svc = hoist::connect_as_service_consumer().unwrap();
            loop {
                let peers = svc.list_peers().await.unwrap();
                for mut peer in peers {
                    if peer.description.services.is_none() {
                        continue;
                    }
                    if peer
                        .description
                        .services
                        .unwrap()
                        .iter()
                        .find(|name| *name == RemoteControlMarker::NAME)
                        .is_none()
                    {
                        continue;
                    }
                    if tc.lock().await.get(peer.id.id.into()).await.is_some() {
                        continue;
                    }
                    let remote_control_proxy = ok_or_continue!(RCSConnection::new(&mut peer.id)
                        .await
                        .context("unable to convert proxy to target"));
                    let target = ok_or_continue!(
                        Target::from_rcs_connection(remote_control_proxy).await,
                        "unable to convert proxy to target",
                    );
                    tc.lock().await.merge_insert(target).await;
                }
            }
        });
    }

    async fn start_remote_control(nodename: &String) -> Result<(), Error> {
        for _ in 0..MAX_RETRY_COUNT {
            let output = Command::new("fx")
            .arg("-d")
            .arg(nodename)
            .arg("run")
            .arg("fuchsia-pkg://fuchsia.com/remote-control-runner#meta/remote-control-runner.cmx")
            .stdin(std::process::Stdio::null())
            .output()
            .context("Failed to run fx")?;
            if output.stdout.starts_with(b"Successfully") {
                return Ok(());
            }
            task::sleep(RETRY_DELAY).await;
        }

        Err(anyhow!("Starting RCS failed. Check target system logs for details."))
    }

    /// Attempts to get at most one target. If there is more than one target,
    /// returns an error.
    /// TODO(fxb/47843): Implement target lookup for commands to deprecate this
    /// function.
    async fn target_from_cache(&self) -> Result<Arc<Target>, Error> {
        let targets = self.target_collection.lock().await;
        if targets.len() > 1 {
            return Err(anyhow!("more than one target"));
        }

        match targets.iter().next() {
            Some(t) => Ok(t.clone()),
            None => Err(anyhow!("no targets found")),
        }
    }

    pub async fn handle_request(&self, req: DaemonRequest, quiet: bool) -> Result<(), Error> {
        match req {
            DaemonRequest::EchoString { value, responder } => {
                if !quiet {
                    log::info!("Received echo request for string {:?}", value);
                }
                responder.send(value.as_ref()).context("error sending response")?;
                if !quiet {
                    log::info!("echo response sent successfully");
                }
            }
            DaemonRequest::StartComponent {
                component_url,
                args,
                component_stdout: stdout,
                component_stderr: stderr,
                controller,
                responder,
            } => {
                if !quiet {
                    log::info!(
                        "Received run component request for string {:?}:{:?}",
                        component_url,
                        args
                    );
                }

                let target = match self.target_from_cache().await {
                    Ok(t) => t,
                    Err(e) => {
                        log::warn!("{}", e);
                        responder
                            .send(&mut Err(ComponentControlError::ComponentControlFailure))
                            .context("sending error response")?;
                        return Ok(());
                    }
                };
                let target_state = target.state.lock().await;
                let mut response = match &target_state.rcs {
                    None => Err(ComponentControlError::ComponentControlFailure),
                    Some(rcs) => {
                        rcs.proxy
                            .start_component(
                                &component_url,
                                &mut args.iter().map(|s| s.as_str()),
                                stdout,
                                stderr,
                                controller,
                            )
                            .await?
                    }
                };
                responder.send(&mut response).context("error sending response")?;
            }
            DaemonRequest::ListTargets { value, responder } => {
                if !quiet {
                    log::info!("Received list target request for '{:?}'", value);
                }
                // TODO(awdavies): Make this into a common format for easy
                // parsing.
                let targets = self.target_collection.lock().await;
                let response = match value.as_ref() {
                    "" => futures::future::join_all(targets.iter().map(|t| t.to_string_async()))
                        .await
                        .join("\n"),
                    _ => format!(
                        "{}",
                        match targets.get(value.into()).await {
                            Some(t) => t.to_string_async().await,
                            None => String::new(),
                        }
                    ),
                };
                responder.send(response.as_ref()).context("error sending response")?;
            }
            DaemonRequest::LaunchSuite { test_url, suite, controller, responder } => {
                if !quiet {
                    log::info!("Received launch suite request for '{:?}'", test_url);
                }
                let target = match self.target_from_cache().await {
                    Ok(t) => t,
                    Err(e) => {
                        log::warn!("{}", e);
                        responder
                            .send(&mut Err(ftest_manager::LaunchError::InternalError))
                            .context("sending error response")?;
                        return Ok(());
                    }
                };
                let target_state = target.state.lock().await;
                match &target_state.rcs {
                    Some(rcs) => match rcs.proxy.launch_suite(&test_url, suite, controller).await {
                        Ok(mut r) => {
                            responder.send(&mut r).context("sending LaunchSuite response")?
                        }
                        Err(_) => responder
                            .send(&mut Err(ftest_manager::LaunchError::InternalError))
                            .context("sending LaunchSuite error")?,
                    },
                    None => {
                        log::warn!("no RCS state available from target '{}'", target.nodename);
                        responder
                            .send(&mut Err(ftest_manager::LaunchError::InternalError))
                            .context("sending LaunchSuite error")?;
                    }
                }
            }
            _ => {
                log::info!("Unsupported method");
            }
        }
        Ok(())
    }
}

////////////////////////////////////////////////////////////////////////////////
// Overnet Server implementation

async fn next_request(
    stream: &mut ServiceProviderRequestStream,
) -> Result<Option<ServiceProviderRequest>, Error> {
    Ok(stream.try_next().await.context("error running service provider server")?)
}

async fn exec_server(daemon: Daemon, quiet: bool) -> Result<(), Error> {
    let (s, p) = fidl::Channel::create().context("failed to create zx channel")?;
    let chan = fidl::AsyncChannel::from_channel(s).context("failed to make async channel")?;
    let mut stream = ServiceProviderRequestStream::from_channel(chan);
    hoist::publish_service(DaemonMarker::NAME, ClientEnd::new(p))?;
    while let Some(ServiceProviderRequest::ConnectToService {
        chan,
        info: _,
        control_handle: _control_handle,
    }) = next_request(&mut stream).await?
    {
        if !quiet {
            log::trace!("Received service request for service");
        }
        let chan =
            fidl::AsyncChannel::from_channel(chan).context("failed to make async channel")?;
        let daemon_clone = daemon.clone();
        spawn(async move {
            daemon_clone
                .handle_requests_from_stream(DaemonRequestStream::from_channel(chan), quiet)
                .await
                .unwrap_or_else(|err| panic!("fatal error handling request: {:?}", err));
        });
    }
    Ok(())
}

////////////////////////////////////////////////////////////////////////////////
// start

pub fn is_daemon_running() -> bool {
    // Try to connect directly to the socket. This will fail if nothing is listening on the other side
    // (even if the path exists).
    match std::os::unix::net::UnixStream::connect(SOCKET) {
        Ok(_) => true,
        Err(_) => false,
    }
}

pub async fn start() -> Result<(), Error> {
    if is_daemon_running() {
        return Ok(());
    }
    log::info!("Starting ascendd");
    start_ascendd().await;
    log::info!("Starting daemon overnet server");
    let daemon = Daemon::new().await?;

    exec_server(daemon, true).await
}

////////////////////////////////////////////////////////////////////////////////
// tests

#[cfg(test)]
mod test {
    use super::*;
    use crate::target::TargetState;
    use chrono::Utc;
    use fidl::endpoints::create_proxy;
    use fidl_fidl_developer_bridge::DaemonMarker;
    use fidl_fuchsia_developer_remotecontrol::{
        ComponentControllerMarker, RemoteControlMarker, RemoteControlProxy, RemoteControlRequest,
    };
    use fidl_fuchsia_overnet_protocol::NodeId;
    use std::collections::HashSet;

    struct TestHookFakeRCS {
        ready_channel: mpsc::UnboundedSender<bool>,
    }

    impl TestHookFakeRCS {
        pub fn new(ready_channel: mpsc::UnboundedSender<bool>) -> Self {
            Self { ready_channel }
        }
    }

    #[async_trait]
    impl DiscoveryHook for TestHookFakeRCS {
        async fn on_new_target(&self, target: &Arc<Target>, _tc: &GuardedTargetCollection) {
            let mut target_state = target.state.lock().await;
            target_state.rcs = match &target_state.rcs {
                Some(_) => panic!("fake RCS should be set at most once"),
                None => Some(RCSConnection::new_with_proxy(
                    setup_fake_target_service(),
                    &NodeId { id: 0u64 },
                )),
            };
            self.ready_channel.unbounded_send(true).unwrap();
        }
    }

    struct TargetControlChannels {
        target_ready_channel: mpsc::UnboundedReceiver<bool>,
        target_detected_channel: mpsc::UnboundedSender<Target>,
    }

    impl TargetControlChannels {
        pub async fn send_target(&mut self, t: Target) {
            self.target_detected_channel.unbounded_send(t).unwrap();
            assert!(self.next_target_ready().await);
        }

        pub async fn next_target_ready(&mut self) -> bool {
            self.target_ready_channel.next().await.unwrap()
        }
    }

    async fn spawn_daemon_server_with_target_ctrl(
        stream: DaemonRequestStream,
    ) -> TargetControlChannels {
        let (target_in, target_out) = mpsc::unbounded::<Target>();
        let (target_ready_channel_in, target_ready_channel_out) = mpsc::unbounded::<bool>();
        spawn(async move {
            let mut d = Daemon::new_with_rx(target_out);
            d.register_hook(TestHookFakeRCS::new(target_ready_channel_in)).await;
            d.handle_requests_from_stream(stream, false)
                .await
                .unwrap_or_else(|err| panic!("Fatal error handling request: {:?}", err));
        });

        TargetControlChannels {
            target_ready_channel: target_ready_channel_out,
            target_detected_channel: target_in,
        }
    }

    async fn spawn_daemon_server_with_fake_target(
        stream: DaemonRequestStream,
    ) -> TargetControlChannels {
        let mut res = spawn_daemon_server_with_target_ctrl(stream).await;
        res.send_target(Target::new("foobar", Utc::now())).await;
        res
    }

    fn setup_fake_target_service() -> RemoteControlProxy {
        let (proxy, mut stream) =
            fidl::endpoints::create_proxy_and_stream::<RemoteControlMarker>().unwrap();

        spawn(async move {
            while let Ok(req) = stream.try_next().await {
                match req {
                    Some(RemoteControlRequest::StartComponent { responder, .. }) => {
                        let _ = responder.send(&mut Ok(())).context("sending ok response");
                    }
                    _ => assert!(false),
                }
            }
        });

        proxy
    }

    #[test]
    fn test_echo() {
        let echo = "test-echo";
        let (daemon_proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<DaemonMarker>().unwrap();
        hoist::run(async move {
            let _ctrl = spawn_daemon_server_with_target_ctrl(stream).await;
            let echoed = daemon_proxy.echo_string(echo).await.unwrap();
            assert_eq!(echoed, echo);
        });
    }

    #[test]
    fn test_start_component() -> Result<(), Error> {
        let url = "fuchsia-pkg://fuchsia.com/test#meta/test.cmx";
        let args = vec!["test1".to_string(), "test2".to_string()];
        let (daemon_proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<DaemonMarker>().unwrap();
        let (_, server_end) = create_proxy::<ComponentControllerMarker>()?;
        let (sout, _) =
            fidl::Socket::create(fidl::SocketOpts::STREAM).context("failed to create socket")?;
        let (serr, _) =
            fidl::Socket::create(fidl::SocketOpts::STREAM).context("failed to create socket")?;
        hoist::run(async move {
            let _ctrl = spawn_daemon_server_with_fake_target(stream).await;
            // There isn't a lot we can test here right now since this method has an empty response.
            // We just check for an Ok(()) and leave it to a real integration test to test behavior.
            daemon_proxy
                .start_component(url, &mut args.iter().map(|s| s.as_str()), sout, serr, server_end)
                .await
                .unwrap()
                .unwrap();
        });

        Ok(())
    }

    #[test]
    fn test_start_component_multiple_targets() -> Result<(), Error> {
        let url = "fuchsia-pkg://fuchsia.com/test#meta/test.cmx";
        let args = vec!["test1".to_string(), "test2".to_string()];
        let (daemon_proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<DaemonMarker>().unwrap();
        let (_, server_end) = create_proxy::<ComponentControllerMarker>()?;
        let (sout, _) =
            fidl::Socket::create(fidl::SocketOpts::STREAM).context("failed to create socket")?;
        let (serr, _) =
            fidl::Socket::create(fidl::SocketOpts::STREAM).context("failed to create socket")?;
        hoist::run(async move {
            let mut ctrl = spawn_daemon_server_with_fake_target(stream).await;
            ctrl.send_target(Target::new("bazmumble", Utc::now())).await;
            match daemon_proxy
                .start_component(url, &mut args.iter().map(|s| s.as_str()), sout, serr, server_end)
                .await
                .unwrap()
            {
                Ok(_) => panic!("failure expected for multiple targets"),
                _ => (),
            }
        });

        Ok(())
    }

    #[test]
    fn test_list_targets() -> Result<(), Error> {
        let (daemon_proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<DaemonMarker>().unwrap();
        hoist::run(async move {
            let mut ctrl = spawn_daemon_server_with_fake_target(stream).await;
            ctrl.send_target(Target::new("baz", Utc::now())).await;
            ctrl.send_target(Target::new("quux", Utc::now())).await;
            let res = daemon_proxy.list_targets("").await.unwrap();

            // TODO(awdavies): This check is in lieu of having an
            // established format for the list_targets output.
            assert!(res.contains("foobar"));
            assert!(res.contains("baz"));
            assert!(res.contains("quux"));

            let res = daemon_proxy.list_targets("mlorp").await.unwrap();
            assert!(!res.contains("foobar"));
            assert!(!res.contains("baz"));
            assert!(!res.contains("quux"));
        });
        Ok(())
    }

    struct TestHookFirst {
        callbacks_done: mpsc::UnboundedSender<bool>,
    }

    #[async_trait]
    impl DiscoveryHook for TestHookFirst {
        async fn on_new_target(&self, target: &Arc<Target>, tc: &GuardedTargetCollection) {
            // This will crash if the target isn't already inserted.
            let t = Arc::clone(&tc.lock().await.get(target.nodename.clone().into()).await.unwrap());
            assert_eq!(t.nodename, "nothin");
            assert_eq!(*t.state.lock().await, TargetState::new());
            assert_eq!(*t.addrs.lock().await, HashSet::new());
            self.callbacks_done.unbounded_send(true).unwrap();
        }
    }

    struct TestHookSecond {
        callbacks_done: mpsc::UnboundedSender<bool>,
    }

    #[async_trait]
    impl DiscoveryHook for TestHookSecond {
        async fn on_new_target(&self, _target: &Arc<Target>, _tc: &GuardedTargetCollection) {
            self.callbacks_done.unbounded_send(true).unwrap();
        }
    }

    #[test]
    fn test_receive_target() {
        hoist::run(async move {
            let (tx_from_callback, mut rx_from_callback) = mpsc::unbounded::<bool>();
            let (tx, rx) = mpsc::unbounded::<Target>();
            let mut daemon = Daemon::new_with_rx(rx);
            daemon.register_hook(TestHookFirst { callbacks_done: tx_from_callback.clone() }).await;
            daemon.register_hook(TestHookSecond { callbacks_done: tx_from_callback }).await;
            tx.unbounded_send(Target::new("nothin", Utc::now())).unwrap();
            assert!(rx_from_callback.next().await.unwrap());
            assert!(rx_from_callback.next().await.unwrap());
        });
    }
}
