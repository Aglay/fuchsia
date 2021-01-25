// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::fidl::FidlServer,
    anyhow::{anyhow, Context, Error},
    config::Config,
    fidl_fuchsia_paver::PaverMarker,
    fuchsia_async as fasync,
    fuchsia_component::server::ServiceFs,
    fuchsia_syslog::{fx_log_info, fx_log_warn},
    fuchsia_zircon::{self as zx, HandleBased},
    futures::{channel::oneshot, prelude::*, stream::FuturesUnordered},
    std::sync::Arc,
};

mod config;
mod fidl;
mod metadata;

pub fn main() -> Result<(), Error> {
    fuchsia_syslog::init_with_tags(&["system-update-committer"])
        .context("while initializing logger")?;
    fx_log_info!("starting system-update-committer");

    let mut executor = fasync::Executor::new().context("error creating executor")?;
    let () = executor.run_singlethreaded(main_inner_async()).map_err(|err| {
        // Use anyhow to print the error chain.
        let err = anyhow!(err);
        fuchsia_syslog::fx_log_err!("error running system-update-committer: {:#}", err);
        err
    })?;

    fx_log_info!("shutting down system-update-committer");
    Ok(())
}

async fn main_inner_async() -> Result<(), Error> {
    // TODO(http://fxbug.dev/64595) Actually use the configuration once we start implementing
    // health verification.
    let _config = Config::load_from_config_data_or_default();

    let paver = fuchsia_component::client::connect_to_service::<PaverMarker>()
        .context("while connecting to paver")?;
    let (boot_manager, boot_manager_server_end) =
        ::fidl::endpoints::create_proxy().context("while creating BootManager endpoints")?;

    paver
        .find_boot_manager(boot_manager_server_end)
        .context("transport error while calling find_boot_manager()")?;

    let futures = FuturesUnordered::new();
    let (p_internal, p_external) = zx::EventPair::create().context("while creating EventPairs")?;

    // Keep a copy of the internal pair so that external consumers don't observe EVENTPAIR_CLOSED.
    let _p_internal_clone = p_internal
        .duplicate_handle(zx::Rights::SIGNAL_PEER | zx::Rights::SIGNAL)
        .context("while duplicating p_internal")?;

    let (unblocker, blocker) = oneshot::channel();

    // Handle putting boot metadata in happy state.
    futures.push(
        async move {
            // TODO(http://fxbug.dev/64595) combine the config and the result of
            // put_metadata_in_happy_state to determine if we should reboot. For now, we just log.
            if let Err(e) =
                crate::metadata::put_metadata_in_happy_state(&boot_manager, &p_internal, unblocker)
                    .await
            {
                fx_log_warn!("error putting boot metadata in happy state: {:#}", anyhow!(e));
            }
        }
        .boxed_local(),
    );

    // Handle FIDL.
    let mut fs = ServiceFs::new_local();
    fs.take_and_serve_directory_handle().context("while serving directory handle")?;
    let fidl = Arc::new(FidlServer::new(p_external, blocker));
    futures.push(FidlServer::run(fidl, fs).boxed_local());

    let () = futures.collect::<()>().await;

    Ok(())
}
