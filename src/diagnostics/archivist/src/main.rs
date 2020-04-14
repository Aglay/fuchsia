// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! The Archivist collects and stores diagnostic data from components.

#![warn(missing_docs)]

use {
    anyhow::{Context, Error},
    archivist_lib::{archivist, configs, diagnostics, logs},
    argh::FromArgs,
    fidl_fuchsia_sys_internal::{ComponentEventProviderMarker, LogConnectorMarker},
    fuchsia_async as fasync,
    fuchsia_component::client::connect_to_service,
    fuchsia_component::server::MissingStartupHandle,
    fuchsia_zircon as zx,
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

    /// serve fuchsia.diagnostics.test.Controller
    #[argh(switch)]
    install_controller: bool,

    /// path to a JSON configuration file
    #[argh(option)]
    config_path: PathBuf,
}

fn main() -> Result<(), Error> {
    let mut executor = fasync::Executor::new()?;

    let event_provider = connect_to_service::<ComponentEventProviderMarker>()
        .context("failed to connect to entity resolver")?;
    diagnostics::init();
    let opt: Args = argh::from_env();

    let archivist_configuration: configs::Config = match configs::parse_config(&opt.config_path) {
        Ok(config) => config,
        Err(parsing_error) => panic!("Parsing configuration failed: {}", parsing_error),
    };

    let num_threads = archivist_configuration.num_threads;

    let mut archivist = archivist::Archivist::new(archivist_configuration)?;
    archivist.install_logger_services().set_event_provider(event_provider);

    if !opt.disable_log_connector {
        archivist.set_log_connector(connect_to_service::<LogConnectorMarker>()?);
    }

    if opt.install_controller {
        archivist.install_controller_service();
    }

    if !opt.disable_klog {
        let log_manager = archivist.log_manager().clone();
        let debug_log = logs::KernelDebugLog::new().context("Failed to read kernel logs")?;
        executor
            .run(async move { log_manager.spawn_debuglog_drainer(debug_log).await }, num_threads)?;
    }

    let startup_handle =
        fuchsia_runtime::take_startup_handle(fuchsia_runtime::HandleType::DirectoryRequest.into())
            .ok_or(MissingStartupHandle)?;

    executor.run(archivist.run(zx::Channel::from(startup_handle)), num_threads)?;
    Ok(())
}
