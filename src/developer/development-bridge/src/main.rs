// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![allow(unused)]
use {
    crate::args::{Ffx, Subcommand, TestCommand},
    crate::config::command::exec_config,
    crate::constants::{CONFIG_JSON_FILE, DAEMON, MAX_RETRY_COUNT},
    anyhow::{anyhow, format_err, Context, Error},
    ffx_daemon::{is_daemon_running, start as start_daemon},
    fidl::endpoints::{create_proxy, ServiceMarker},
    fidl_fidl_developer_bridge::{DaemonMarker, DaemonProxy},
    fidl_fuchsia_developer_remotecontrol::{ComponentControllerEvent, ComponentControllerMarker},
    fidl_fuchsia_overnet::ServiceConsumerProxyInterface,
    fidl_fuchsia_overnet_protocol::NodeId,
    fidl_fuchsia_test::{CaseIteratorMarker, SuiteProxy},
    futures::{channel::mpsc, FutureExt, StreamExt, TryStreamExt},
    signal_hook,
    std::env,
    std::process::Command,
    std::sync::{Arc, Mutex},
    test_executor::{run_and_collect_results as run_tests, TestEvent, TestResult},
};

mod args;
mod config;
mod constants;

// Cli
pub struct Cli {
    daemon_proxy: DaemonProxy,
}

impl Cli {
    pub async fn new() -> Result<Cli, Error> {
        let mut peer_id = Cli::find_daemon().await?;
        let daemon_proxy = Cli::create_daemon_proxy(&mut peer_id).await?;
        Ok(Cli { daemon_proxy })
    }

    pub fn new_with_proxy(daemon_proxy: DaemonProxy) -> Cli {
        Cli { daemon_proxy }
    }

    async fn create_daemon_proxy(id: &mut NodeId) -> Result<DaemonProxy, Error> {
        let svc = hoist::connect_as_service_consumer()?;
        let (s, p) = fidl::Channel::create().context("failed to create zx channel")?;
        svc.connect_to_service(id, DaemonMarker::NAME, s)?;
        let proxy = fidl::AsyncChannel::from_channel(p).context("failed to make async channel")?;
        Ok(DaemonProxy::new(proxy))
    }

    async fn find_daemon() -> Result<NodeId, Error> {
        if !is_daemon_running() {
            Cli::spawn_daemon().await?;
        }
        let svc = hoist::connect_as_service_consumer()?;
        // Sometimes list_peers doesn't properly report the published services - retry a few times
        // but don't loop indefinitely.
        for _ in 0..MAX_RETRY_COUNT {
            let peers = svc.list_peers().await?;
            log::trace!("Got peers: {:?}", peers);
            for peer in peers {
                if peer.description.services.is_none() {
                    continue;
                }
                if peer
                    .description
                    .services
                    .unwrap()
                    .iter()
                    .find(|name| *name == DaemonMarker::NAME)
                    .is_none()
                {
                    continue;
                }
                return Ok(peer.id);
            }
        }
        panic!("No daemon found.")
    }

    pub async fn echo(&self, text: Option<String>) -> Result<String, Error> {
        match self
            .daemon_proxy
            .echo_string(match text {
                Some(ref t) => t,
                None => "Ffx",
            })
            .await
        {
            Ok(r) => {
                log::info!("SUCCESS: received {:?}", r);
                return Ok(r);
            }
            Err(e) => panic!("ERROR: {:?}", e),
        }
    }

    pub async fn list_targets(&self, text: Option<String>) -> Result<String, Error> {
        match self
            .daemon_proxy
            .list_targets(match text {
                Some(ref t) => t,
                None => "",
            })
            .await
        {
            Ok(r) => {
                log::info!("SUCCESS: received {:?}", r);
                return Ok(r);
            }
            Err(e) => panic!("ERROR: {:?}", e),
        }
    }

    pub async fn run_component(&self, url: String, args: &Vec<String>) -> Result<(), Error> {
        let (proxy, server_end) = create_proxy::<ComponentControllerMarker>()?;
        let (sout, cout) =
            fidl::Socket::create(fidl::SocketOpts::STREAM).context("failed to create socket")?;
        let (serr, cerr) =
            fidl::Socket::create(fidl::SocketOpts::STREAM).context("failed to create socket")?;

        // This is only necessary until Overnet correctly handle setup for passed channels.
        // TODO(jwing) remove this once that is finished.
        proxy.ping();

        let out_thread = std::thread::spawn(move || loop {
            let mut buf = [0u8; 128];
            let n = cout.read(&mut buf).or::<usize>(Ok(0usize)).unwrap();
            if n > 0 {
                print!("{}", String::from_utf8_lossy(&buf));
            }
        });

        let err_thread = std::thread::spawn(move || loop {
            let mut buf = [0u8; 128];
            let n = cerr.read(&mut buf).or::<usize>(Ok(0usize)).unwrap();
            if n > 0 {
                eprint!("{}", String::from_utf8_lossy(&buf));
            }
        });

        let event_stream = proxy.take_event_stream();
        let term_thread = std::thread::spawn(move || {
            let mut e = event_stream.take(1usize);
            while let Some(result) = futures::executor::block_on(e.next()) {
                match result {
                    Ok(ComponentControllerEvent::OnTerminated { exit_code }) => {
                        println!("Component exited with exit code: {}", exit_code);
                        match exit_code {
                            -1 => println!("This exit code may mean that the specified package doesn't exist.\
                                        \nCheck that the package is in your universe (`fx set --with ...`) and that `fx serve` is running."),
                            _ => {}
                        };
                        break;
                    }
                    Err(err) => {
                        eprintln!("error reading component controller events. Component termination may not be detected correctly. {} ", err);
                    }
                }
            }
        });

        let kill_arc = Arc::new(Mutex::new(false));
        let arc_mut = kill_arc.clone();
        unsafe {
            signal_hook::register(signal_hook::SIGINT, move || {
                let mut kill_started = arc_mut.lock().unwrap();
                if !*kill_started {
                    println!("\nCaught interrupt, killing remote component.");
                    proxy.kill();
                    *kill_started = true;
                } else {
                    // If for some reason the kill signal hangs, we want to give the user
                    // a way to exit ffx.
                    println!("Received second interrupt. Forcing exit...");
                    std::process::exit(0);
                }
            });
        }

        let _result = self
            .daemon_proxy
            .start_component(&url, &mut args.iter().map(|s| s.as_str()), sout, serr, server_end)
            .await?;
        term_thread.join().unwrap();

        Ok(())
    }

    async fn get_tests(&self, suite_url: &String) -> Result<(), Error> {
        let (suite_proxy, suite_server_end) = fidl::endpoints::create_proxy().unwrap();
        let (_controller_proxy, controller_server_end) = fidl::endpoints::create_proxy().unwrap();

        log::info!("launching test suite {}", suite_url);

        self.daemon_proxy
            .launch_suite(&suite_url, suite_server_end, controller_server_end)
            .await
            .context("launch_test call failed")?
            .map_err(|e| format_err!("error launching test: {:?}", e))?;

        log::info!("launched suite, getting tests");

        let (case_iterator, test_server_end) = create_proxy::<CaseIteratorMarker>()?;
        suite_proxy
            .get_tests(test_server_end)
            .map_err(|e| format_err!("Error getting test steps: {}", e))?;

        loop {
            let cases = case_iterator.get_next().await?;
            if cases.is_empty() {
                return Ok(());
            }
            println!("Tests in suite {}:\n", suite_url);
            for case in cases {
                match case.name {
                    Some(n) => println!("{}", n),
                    None => println!("<No name>"),
                }
            }
        }
    }

    async fn run_tests(&self, suite_url: &String) -> Result<(), Error> {
        let (suite_proxy, suite_server_end) =
            fidl::endpoints::create_proxy().expect("creating suite proxy");
        let (_controller_proxy, controller_server_end) =
            fidl::endpoints::create_proxy().expect("creating controller proxy");

        log::info!("launching test suite {}", suite_url);

        self.daemon_proxy
            .launch_suite(&suite_url, suite_server_end, controller_server_end)
            .await
            .context("launch_test call failed")?
            .map_err(|e| format_err!("error launching test: {:?}", e))?;

        log::info!("launched suite, getting tests");
        let (sender, recv) = mpsc::channel(1);

        let (remote, test_fut) =
            run_tests(suite_proxy, sender, suite_url.to_string()).remote_handle();

        println!("*** Running {} ***", suite_url);
        hoist::spawn(remote);

        let mut successful_completion = false;
        let events = recv.collect::<Vec<_>>().await;
        for event in events {
            match event {
                TestEvent::LogMessage { test_case_name, msg } => {
                    let logs = msg.split("\n");
                    for log in logs {
                        if log.len() > 0 {
                            println!("{}: {}", test_case_name, log.to_string());
                        }
                    }
                }
                TestEvent::TestCaseStarted { test_case_name } => {
                    println!("{} started ...", test_case_name);
                }
                TestEvent::TestCaseFinished { test_case_name, result } => {
                    match result {
                        TestResult::Passed => println!("{} PASSED", test_case_name),
                        TestResult::Failed => println!("{} FAILED", test_case_name),
                        TestResult::Skipped => println!("{} SKIPPED", test_case_name),
                        TestResult::Error => println!("{} ERROR", test_case_name),
                    };
                }
                TestEvent::Finish => {
                    successful_completion = true;
                    println!("*** Finished {} ***", suite_url);
                }
            };
        }

        test_fut.await.map_err(|e| format_err!("Error running test: {}", e))?;

        if !successful_completion {
            return Err(anyhow!("Test run finished prematurely. Something went wrong."));
        }
        Ok(())
    }

    pub async fn test(&self, test: TestCommand) -> Result<(), Error> {
        if (test.list) {
            self.get_tests(&test.url).await
        } else {
            self.run_tests(&test.url).await
        }
    }

    async fn spawn_daemon() -> Result<(), Error> {
        Command::new(env::current_exe().unwrap()).arg(DAEMON).spawn()?;
        Ok(())
    }
}

////////////////////////////////////////////////////////////////////////////////
// main

async fn async_main() -> Result<(), Error> {
    let app: Ffx = argh::from_env();
    match app.subcommand {
        Subcommand::Echo(c) => {
            match Cli::new().await?.echo(c.text).await {
                Ok(r) => {
                    println!("SUCCESS: received {:?}", r);
                }
                Err(e) => {
                    println!("ERROR: {:?}", e);
                }
            }
            Ok(())
        }
        Subcommand::List(c) => {
            match Cli::new().await?.list_targets(c.nodename).await {
                Ok(r) => {
                    println!("SUCCESS: received {:?}", r);
                }
                Err(e) => {
                    println!("ERROR: {:?}", e);
                }
            }
            Ok(())
        }
        Subcommand::RunComponent(c) => {
            match Cli::new().await?.run_component(c.url, &c.args).await {
                Ok(r) => {}
                Err(e) => {
                    println!("ERROR: {:?}", e);
                }
            }
            Ok(())
        }
        Subcommand::Daemon(_) => start_daemon().await,
        Subcommand::Config(c) => exec_config(c),
        Subcommand::Test(t) => {
            match Cli::new().await.unwrap().test(t).await {
                Ok(_) => {
                    log::info!("Test successfully run");
                }
                Err(e) => {
                    println!("ERROR: {:?}", e);
                }
            }
            Ok(())
        }
    }
}

fn main() {
    hoist::run(async move {
        async_main().await.map_err(|e| println!("{}", e)).expect("could not start ffx");
    })
}

////////////////////////////////////////////////////////////////////////////////
// tests

#[cfg(test)]
mod test {
    use super::*;
    use fidl_fidl_developer_bridge::{DaemonMarker, DaemonProxy, DaemonRequest};
    use fidl_fuchsia_test::{
        Case, CaseIteratorRequest, CaseIteratorRequestStream, SuiteRequest, SuiteRequestStream,
    };

    fn spawn_fake_iterator_server(
        values: Vec<&'static str>,
        mut stream: CaseIteratorRequestStream,
    ) {
        let mut iter = values.into_iter().map(|name| Case { name: Some(name.to_string()) });
        hoist::spawn(async move {
            while let Ok(req) = stream.try_next().await {
                match req {
                    Some(CaseIteratorRequest::GetNext { responder }) => {
                        responder.send(&mut iter.by_ref().take(50));
                    }
                    _ => assert!(false),
                }
            }
        });
    }

    fn spawn_fake_suite_server(mut stream: SuiteRequestStream) {
        hoist::spawn(async move {
            while let Ok(req) = stream.try_next().await {
                match req {
                    Some(SuiteRequest::GetTests { iterator, control_handle: _ }) => {
                        let values = vec!["Test 1", "Test 2"];
                        let iterator_request_stream = iterator.into_stream().unwrap();
                        spawn_fake_iterator_server(values, iterator_request_stream);
                    }
                    Some(SuiteRequest::Run { tests, options: _, listener, .. }) => {
                        let listener = listener
                            .into_proxy()
                            .context("Can't convert listener into proxy")
                            .unwrap();
                        listener.on_finished().context("Cannot send on_finished event").unwrap();
                    }
                    _ => assert!(false),
                }
            }
        });
    }

    fn setup_fake_daemon_service() -> DaemonProxy {
        let (proxy, mut stream) =
            fidl::endpoints::create_proxy_and_stream::<DaemonMarker>().unwrap();

        hoist::spawn(async move {
            while let Ok(req) = stream.try_next().await {
                match req {
                    Some(DaemonRequest::EchoString { value, responder }) => {
                        let _ = responder.send(value.as_ref());
                    }
                    Some(DaemonRequest::StartComponent {
                        component_url,
                        args,
                        component_stdout: _,
                        component_stderr: _,
                        controller: _,
                        responder,
                    }) => {
                        let _ = responder.send(&mut Ok(()));
                    }
                    Some(DaemonRequest::LaunchSuite { test_url, suite, controller, responder }) => {
                        let suite_request_stream = suite.into_stream().unwrap();
                        spawn_fake_suite_server(suite_request_stream);
                        let _ = responder.send(&mut Ok(()));
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
        hoist::run(async move {
            let echoed = Cli::new_with_proxy(setup_fake_daemon_service())
                .echo(Some(echo.to_string()))
                .await
                .unwrap();
            assert_eq!(echoed, echo);
        });
    }

    #[test]
    fn test_run_component() -> Result<(), Error> {
        let url = "fuchsia-pkg://fuchsia.com/test#meta/test.cmx";
        let args = vec!["test1".to_string(), "test2".to_string()];
        let (daemon_proxy, stream) = fidl::endpoints::create_proxy_and_stream::<DaemonMarker>()?;
        let (_, server_end) = create_proxy::<ComponentControllerMarker>()?;
        let (sout, _) =
            fidl::Socket::create(fidl::SocketOpts::STREAM).context("failed to create socket")?;
        let (serr, _) =
            fidl::Socket::create(fidl::SocketOpts::STREAM).context("failed to create socket")?;
        hoist::run(async move {
            // There isn't a lot we can test here right now since this method has an empty response.
            // We just check for an Ok(()) and leave it to a real integration to test behavior.
            let response = Cli::new_with_proxy(setup_fake_daemon_service())
                .run_component(url.to_string(), &args)
                .await
                .unwrap();
        });

        Ok(())
    }

    #[test]
    fn test_list_tests() -> Result<(), Error> {
        let url = "fuchsia-pkg://fuchsia.com/gtest_adapter_echo_example#meta/echo_test_realm.cm"
            .to_string();
        let cmd = TestCommand { url, devices: None, list: true };
        hoist::run(async move {
            let response =
                Cli::new_with_proxy(setup_fake_daemon_service()).test(cmd).await.unwrap();
            assert_eq!(response, ());
        });
        Ok(())
    }

    #[test]
    fn test_run_tests() -> Result<(), Error> {
        let url = "fuchsia-pkg://fuchsia.com/gtest_adapter_echo_example#meta/echo_test_realm.cm"
            .to_string();
        let cmd = TestCommand { url, devices: None, list: false };
        hoist::run(async move {
            let response =
                Cli::new_with_proxy(setup_fake_daemon_service()).test(cmd).await.unwrap();
            assert_eq!(response, ());
        });
        Ok(())
    }
}
