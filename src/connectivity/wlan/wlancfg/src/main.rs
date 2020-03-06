// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![recursion_limit = "512"]

mod client;
mod config;
mod config_manager;
mod device;
mod fuse_pending;
mod known_ess_store;
mod network_config;
mod policy;
mod shim;
mod stash;
mod state_machine;
#[cfg(test)]
mod testutils;
mod util;

use {
    crate::{config::Config, config_manager::SavedNetworksManager},
    anyhow::{format_err, Context as _, Error},
    fidl_fuchsia_wlan_device_service::DeviceServiceMarker,
    fuchsia_async as fasync,
    fuchsia_component::server::ServiceFs,
    fuchsia_syslog::{self as syslog},
    futures::{channel::mpsc, future::try_join, prelude::*, select},
    parking_lot::Mutex,
    pin_utils::pin_mut,
    std::sync::Arc,
    void::Void,
};

async fn serve_fidl(
    _client_ref: shim::ClientRef,
    saved_networks: Arc<SavedNetworksManager>,
) -> Result<Void, Error> {
    let mut fs = ServiceFs::new();
    let (listener_msg_sender, listener_msgs) = mpsc::unbounded();
    let listener_msg_sender1 = listener_msg_sender.clone();
    let listener_msg_sender2 = listener_msg_sender.clone();
    let saved_networks_clone = Arc::clone(&saved_networks);
    fs.dir("svc")
        .add_fidl_service(|stream| {
            let fut = shim::serve_legacy(stream, _client_ref.clone(), Arc::clone(&saved_networks))
                .unwrap_or_else(|e| eprintln!("error serving legacy wlan API: {}", e));
            fasync::spawn(fut)
        })
        .add_fidl_service(move |reqs| {
            policy::client::spawn_provider_server(
                Arc::new(Mutex::new(policy::client::Client::new_empty())),
                listener_msg_sender1.clone(),
                Arc::clone(&saved_networks_clone),
                reqs,
            )
        })
        .add_fidl_service(move |reqs| {
            policy::client::spawn_listener_server(listener_msg_sender2.clone(), reqs)
        });
    fs.take_and_serve_directory_handle()?;
    let service_fut = fs.collect::<()>().fuse();
    pin_mut!(service_fut);

    let serve_policy_listeners = policy::client::listener::serve(listener_msgs).fuse();
    pin_mut!(serve_policy_listeners);

    loop {
        select! {
            _ = service_fut => (),
            _ = serve_policy_listeners => (),
        }
    }
}

fn main() -> Result<(), Error> {
    syslog::init_with_tags(&["wlan-policy"]).expect("Should not fail");
    let cfg = Config::load_from_file()?;

    let mut executor = fasync::Executor::new().context("error create event loop")?;
    let wlan_svc = fuchsia_component::client::connect_to_service::<DeviceServiceMarker>()
        .context("failed to connect to device service")?;

    let saved_networks = Arc::new(executor.run_singlethreaded(SavedNetworksManager::new())?);
    let legacy_client = shim::ClientRef::new();
    let fidl_fut = serve_fidl(legacy_client.clone(), Arc::clone(&saved_networks));

    let (watcher_proxy, watcher_server_end) = fidl::endpoints::create_proxy()?;
    wlan_svc.watch_devices(watcher_server_end)?;
    let listener = device::Listener::new(wlan_svc, cfg, legacy_client);
    let fut = watcher_proxy
        .take_event_stream()
        .try_for_each(|evt| {
            device::handle_event(&listener, evt, Arc::clone(&saved_networks)).map(Ok)
        })
        .err_into()
        .and_then(|_| future::ready(Err(format_err!("Device watcher future exited unexpectedly"))));

    executor.run_singlethreaded(try_join(fidl_fut, fut)).map(|_: (Void, Void)| ())
}
