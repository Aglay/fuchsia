// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! The Archivist collects and stores diagnostic data from components.

#![warn(missing_docs)]

use {
    anyhow::{Context, Error},
    archivist_lib::{archivist, configs, diagnostics, logs},
    argh::FromArgs,
    fidl_fuchsia_sys2::EventSourceMarker,
    fidl_fuchsia_sys_internal::{ComponentEventProviderMarker, LogConnectorMarker},
    fuchsia_async as fasync,
    fuchsia_component::client::connect_to_service,
    fuchsia_component::server::MissingStartupHandle,
    fuchsia_syslog, fuchsia_zircon as zx,
    log::{info, warn},
    std::path::PathBuf,
};

/// Monitor, collect, and store diagnostics from components.
#[derive(Debug, Default, FromArgs)]
pub struct Args {
    /// disables proxying kernel logger
    #[argh(switch)]
    disable_klog: bool,

    /// disables log connector so that indivisual instances of
    /// observer don't compete for log connector listener.
    #[argh(switch)]
    disable_log_connector: bool,

    /// initializes syslog library with a log socket to itself
    #[argh(switch)]
    consume_own_logs: bool,

    /// serve fuchsia.diagnostics.test.Controller
    #[argh(switch)]
    install_controller: bool,

    /// path to a JSON configuration file
    #[argh(option)]
    config_path: PathBuf,
}

fn main() -> Result<(), Error> {
    let opt: Args = argh::from_env();

    let log_name;
    let mut log_server = None;
    if opt.consume_own_logs {
        let (log_client, server) = zx::Socket::create(zx::SocketOpts::DATAGRAM)?;
        log_server = Some(server);
        log_name = "archivist";
        fuchsia_syslog::init_with_socket_and_name(log_client, log_name)?;
        info!("Logging started.");
    } else {
        log_name = "observer";
        fuchsia_syslog::init_with_tags(&[log_name])?;
    }

    let mut executor = fasync::Executor::new()?;

    let legacy_event_provider = connect_to_service::<ComponentEventProviderMarker>()
        .context("failed to connect to event provider")?;
    let event_source =
        connect_to_service::<EventSourceMarker>().context("failed to connect to event source")?;

    diagnostics::init();

    let archivist_configuration: configs::Config = match configs::parse_config(&opt.config_path) {
        Ok(config) => config,
        Err(parsing_error) => panic!("Parsing configuration failed: {}", parsing_error),
    };

    let num_threads = archivist_configuration.num_threads;

    let mut archivist = archivist::Archivist::new(archivist_configuration)?;
    archivist
        .install_logger_services()
        .add_event_source("v1", Box::new(legacy_event_provider))
        .add_event_source("v2", Box::new(event_source));
    if let Some(log_server) = log_server {
        fasync::Task::spawn(
            archivist.log_manager().clone().drain_internal_log_sink(log_server, log_name),
        )
        .detach();
    }

    if opt.install_controller {
        archivist.install_controller_service();
    }

    if !opt.disable_log_connector {
        let connector = connect_to_service::<LogConnectorMarker>()?;
        let sender = archivist.log_sender().clone();
        fasync::Task::spawn(
            archivist.log_manager().clone().handle_log_connector(connector, sender),
        )
        .detach();
    }

    if !opt.disable_klog {
        let debuglog = executor
            .run_singlethreaded(logs::KernelDebugLog::new())
            .context("Failed to read kernel logs")?;
        fasync::Task::spawn(archivist.log_manager().clone().drain_debuglog(debuglog)).detach();
    }

    let startup_handle =
        fuchsia_runtime::take_startup_handle(fuchsia_runtime::HandleType::DirectoryRequest.into())
            .ok_or(MissingStartupHandle)?;

    executor.run(archivist.run(zx::Channel::from(startup_handle)), num_threads)
}
