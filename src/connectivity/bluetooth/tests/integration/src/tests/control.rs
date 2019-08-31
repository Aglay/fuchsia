// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    failure::{format_err, Error},
    fidl_fuchsia_bluetooth_test::{AdvertisingData, LowEnergyPeerParameters},
    fuchsia_bluetooth::{
        expectation::{
            self,
            asynchronous::{ExpectableState, ExpectableStateExt},
            Predicate,
        },
        hci_emulator::Emulator,
        types::Address,
    },
};

use crate::harness::control::{
    activate_fake_host, control_expectation, control_timeout, ControlHarness, ControlState,
    FAKE_HCI_ADDRESS,
};

async fn test_set_active_host(control: ControlHarness) -> Result<(), Error> {
    let initial_hosts: Vec<String> = control.read().hosts.keys().cloned().collect();
    let initial_hosts_ = initial_hosts.clone();

    let fake_hci_0 = Emulator::create_and_publish("bt-hci-integration-control-0").await?;
    let fake_hci_1 = Emulator::create_and_publish("bt-hci-integration-control-1").await?;

    let state = control
        .when_satisfied(
            Predicate::<ControlState>::new(
                move |control| {
                    let added_fake_hosts = control.hosts.iter().filter(|(id, host)| {
                        host.address == FAKE_HCI_ADDRESS && !initial_hosts_.contains(id)
                    });
                    added_fake_hosts.count() > 1
                },
                Some("Both Fake Hosts Added"),
            ),
            control_timeout(),
        )
        .await?;

    let fake_hosts: Vec<String> = state
        .hosts
        .iter()
        .filter(|(id, host)| host.address == FAKE_HCI_ADDRESS && !initial_hosts.contains(id))
        .map(|(id, _)| id)
        .cloned()
        .collect();

    for (id, _) in state.hosts {
        control.aux().set_active_adapter(&id).await?;
        control.when_satisfied(control_expectation::active_host_is(id), control_timeout()).await?;
    }

    drop(fake_hci_0);
    drop(fake_hci_1);

    for host in fake_hosts {
        control
            .when_satisfied(control_expectation::host_not_present(host), control_timeout())
            .await?;
    }

    Ok(())
}

async fn test_disconnect(control: ControlHarness) -> Result<(), Error> {
    let (_host, hci) = activate_fake_host(control.clone(), "bt-hci-integration").await?;

    // Insert a fake peer to test connection and disconnection.
    let peer_address = Address::Random([1, 0, 0, 0, 0, 0]);
    let peer_address_string = peer_address.to_string();
    let peer_params = LowEnergyPeerParameters {
        address: Some(peer_address.into()),
        connectable: Some(true),
        advertisement: Some(AdvertisingData {
            data: vec![0x02, 0x01, 0x02], // Flags field set to "general discoverable"
        }),
        scan_response: None,
    };
    let (_peer, remote) = fidl::endpoints::create_proxy()?;
    let _ = hci
        .emulator()
        .add_low_energy_peer(peer_params, remote)
        .await?
        .map_err(|e| format_err!("Failed to register fake peer: {:#?}", e))?;

    control.aux().request_discovery(true).await?;
    let state = control
        .when_satisfied(
            control_expectation::peer_exists(expectation::peer::address(&peer_address_string)),
            control_timeout(),
        )
        .await?;

    // We can safely unwrap here as this is guarded by the previous expectation
    let peer = state.peers.iter().find(|(_, d)| &d.address == &peer_address_string).unwrap().0;

    control.aux().connect(peer).await?;

    control
        .when_satisfied(control_expectation::peer_connected(peer, true), control_timeout())
        .await?;
    control.aux().disconnect(peer).await?;

    control
        .when_satisfied(control_expectation::peer_connected(peer, false), control_timeout())
        .await?;
    Ok(())
}

/// Run all test cases.
pub fn run_all() -> Result<(), Error> {
    run_suite!("control.Control", [test_set_active_host, test_disconnect])
}
