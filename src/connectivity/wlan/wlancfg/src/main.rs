// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![recursion_limit = "1024"]

mod access_point;
mod client;
mod config_management;
mod legacy;
mod mode_management;
mod util;

#[cfg(test)]
mod regulatory_manager;

use {
    crate::{
        client::{
            connect_to_best_network, handle_client_state_machine_event,
            network_selection::NetworkSelector, scan_for_network_selector,
            state_machine as client_fsm,
        },
        config_management::SavedNetworksManager,
        legacy::{device, shim},
        mode_management::{
            iface_manager::{IfaceManager, IfaceManagerApi},
            phy_manager::PhyManager,
        },
    },
    anyhow::{format_err, Context as _, Error},
    fidl_fuchsia_wlan_device_service::DeviceServiceMarker,
    fidl_fuchsia_wlan_policy as fidl_policy,
    fuchsia_async::{self as fasync, DurationExt},
    fuchsia_component::server::ServiceFs,
    fuchsia_zircon::prelude::*,
    futures::{
        self, channel::mpsc, future::try_join, lock::Mutex, prelude::*, select, TryFutureExt,
    },
    log::{error, info},
    pin_utils::pin_mut,
    std::sync::Arc,
    void::Void,
};

// Value taken from legacy state machine.
const AUTO_CONNECT_RETRY_SECONDS: i64 = 10;

async fn monitor_client_events(
    iface_manager: Arc<Mutex<dyn IfaceManagerApi + Send>>,
    selector: Arc<NetworkSelector>,
    mut client_events: mpsc::Receiver<client_fsm::ClientStateMachineNotification>,
) {
    loop {
        match client_events.next().await {
            Some(event) => {
                handle_client_state_machine_event(
                    event,
                    Arc::clone(&iface_manager),
                    Arc::clone(&selector),
                )
                .await;
            }
            None => break,
        }
    }
}

async fn monitor_client_connectivity(
    iface_manager: Arc<Mutex<dyn IfaceManagerApi + Send>>,
    saved_networks: Arc<SavedNetworksManager>,
    selector: Arc<NetworkSelector>,
) {
    loop {
        fasync::Timer::new(AUTO_CONNECT_RETRY_SECONDS.seconds().after_now()).await;

        if saved_networks.known_network_count() == 0 {
            // No saved networks, autoconnect won't succeed. Don't perform a scan/connection attempt
            continue;
        }

        let temp_iface_manager = iface_manager.clone();
        let temp_iface_manager = temp_iface_manager.lock().await;
        if temp_iface_manager.has_idle_client() {
            drop(temp_iface_manager);
            info!("Detected idle interface, scanning to allow automatic reconnect");
            scan_for_network_selector(iface_manager.clone(), selector.clone()).await;

            // TODO(fxb/54046): Centralize the calls that reconnect a disconnected client.
            connect_to_best_network(iface_manager.clone(), selector.clone()).await;
        }
    }
}

async fn serve_fidl(
    ap: access_point::AccessPoint,
    configurator: legacy::deprecated_configuration::DeprecatedConfigurator,
    iface_manager: Arc<Mutex<dyn IfaceManagerApi + Send>>,
    legacy_client_ref: shim::IfaceRef,
    saved_networks: Arc<SavedNetworksManager>,
    network_selector: Arc<NetworkSelector>,
    client_sender: util::listener::ClientListenerMessageSender,
    client_listener_msgs: mpsc::UnboundedReceiver<util::listener::ClientListenerMessage>,
    ap_listener_msgs: mpsc::UnboundedReceiver<util::listener::ApMessage>,
    client_events: mpsc::Receiver<client_fsm::ClientStateMachineNotification>,
) -> Result<Void, Error> {
    let mut fs = ServiceFs::new();
    let client_sender1 = client_sender.clone();
    let client_sender2 = client_sender.clone();

    let second_ap = ap.clone();

    let saved_networks_clone = saved_networks.clone();

    let cloned_iface_manager = iface_manager.clone();
    let cloned_selector = network_selector.clone();

    // TODO(sakuma): Once the legacy API is deprecated, the interface manager should default to
    // stopped.
    {
        let mut iface_manager = iface_manager.lock().await;
        iface_manager.start_client_connections().await?;
    }

    fs.dir("svc")
        .add_fidl_service(|stream| {
            let fut = shim::serve_legacy(stream, legacy_client_ref.clone(), saved_networks.clone())
                .unwrap_or_else(|e| error!("error serving legacy wlan API: {}", e));
            fasync::spawn(fut)
        })
        .add_fidl_service(move |reqs| {
            client::spawn_provider_server(
                iface_manager.clone(),
                client_sender1.clone(),
                Arc::clone(&saved_networks_clone),
                Arc::clone(&network_selector),
                reqs,
            )
        })
        .add_fidl_service(move |reqs| client::spawn_listener_server(client_sender2.clone(), reqs))
        .add_fidl_service(move |reqs| fasync::spawn(ap.clone().serve_provider_requests(reqs)))
        .add_fidl_service(move |reqs| {
            fasync::spawn(second_ap.clone().serve_listener_requests(reqs))
        })
        .add_fidl_service(move |reqs| {
            fasync::spawn(configurator.clone().serve_deprecated_configuration(reqs))
        });
    fs.take_and_serve_directory_handle()?;
    let service_fut = fs.collect::<()>().fuse();
    pin_mut!(service_fut);

    let serve_client_policy_listeners = util::listener::serve::<
        fidl_policy::ClientStateUpdatesProxy,
        fidl_policy::ClientStateSummary,
        util::listener::ClientStateUpdate,
    >(client_listener_msgs)
    .fuse();
    pin_mut!(serve_client_policy_listeners);

    let serve_ap_policy_listeners = util::listener::serve::<
        fidl_policy::AccessPointStateUpdatesProxy,
        Vec<fidl_policy::AccessPointState>,
        util::listener::ApStatesUpdate,
    >(ap_listener_msgs)
    .fuse();
    pin_mut!(serve_ap_policy_listeners);

    let client_event_monitor =
        monitor_client_events(cloned_iface_manager.clone(), cloned_selector.clone(), client_events)
            .fuse();
    pin_mut!(client_event_monitor);

    let client_connectivity_monitor = monitor_client_connectivity(
        cloned_iface_manager.clone(),
        saved_networks.clone(),
        cloned_selector.clone(),
    )
    .fuse();
    pin_mut!(client_connectivity_monitor);

    loop {
        select! {
            _ = client_connectivity_monitor => (),
            _ = client_event_monitor => (),
            _ = service_fut => (),
            _ = serve_client_policy_listeners => (),
            _ = serve_ap_policy_listeners => (),
        }
    }
}

fn main() -> Result<(), Error> {
    util::logger::init();

    let mut executor = fasync::Executor::new().context("error create event loop")?;
    let wlan_svc = fuchsia_component::client::connect_to_service::<DeviceServiceMarker>()
        .context("failed to connect to device service")?;

    let saved_networks = Arc::new(executor.run_singlethreaded(SavedNetworksManager::new())?);
    let network_selector = Arc::new(NetworkSelector::new(Arc::clone(&saved_networks)));
    let phy_manager = Arc::new(Mutex::new(PhyManager::new(wlan_svc.clone())));
    let configurator =
        legacy::deprecated_configuration::DeprecatedConfigurator::new(phy_manager.clone());

    let (watcher_proxy, watcher_server_end) = fidl::endpoints::create_proxy()?;
    wlan_svc.watch_devices(watcher_server_end)?;

    let (client_sender, client_receiver) = mpsc::unbounded();
    let (ap_sender, ap_receiver) = mpsc::unbounded();
    let (client_event_sender, client_event_receiver) = mpsc::channel(0);
    let iface_manager = Arc::new(Mutex::new(IfaceManager::new(
        phy_manager.clone(),
        client_sender.clone(),
        ap_sender.clone(),
        wlan_svc.clone(),
        saved_networks.clone(),
        client_event_sender,
    )));

    let legacy_client = shim::IfaceRef::new();
    let listener = device::Listener::new(
        wlan_svc.clone(),
        legacy_client.clone(),
        phy_manager.clone(),
        iface_manager.clone(),
    );

    let ap = access_point::AccessPoint::new(iface_manager.clone(), ap_sender);
    let fidl_fut = serve_fidl(
        ap,
        configurator,
        iface_manager.clone(),
        legacy_client,
        saved_networks.clone(),
        network_selector,
        client_sender,
        client_receiver,
        ap_receiver,
        client_event_receiver,
    );

    let fut = watcher_proxy
        .take_event_stream()
        .try_for_each(|evt| device::handle_event(&listener, evt).map(Ok))
        .err_into()
        .and_then(|_| future::ready(Err(format_err!("Device watcher future exited unexpectedly"))));

    executor.run_singlethreaded(try_join(fidl_fut, fut)).map(|_: (Void, Void)| ())
}
