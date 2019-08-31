// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    failure::{err_msg, format_err, Error, Fail, ResultExt},
    fidl_fuchsia_bluetooth_test::{
        AdvertisingData, HciEmulatorProxy, LowEnergyPeerParameters, PeerProxy,
    },
    fuchsia_async::{DurationExt, TimeoutExt},
    fuchsia_bluetooth::{
        error::Error as BTError, expectation::asynchronous::ExpectableStateExt, types::Address,
    },
    fuchsia_zircon::{Duration, DurationNum},
    futures::TryFutureExt,
};

use crate::harness::low_energy_central::CentralHarness;

mod central_expectation {
    use crate::harness::low_energy_central::{CentralState, ScanStateChange};
    use fuchsia_bluetooth::expectation::Predicate;
    use fuchsia_bluetooth::types::le::RemoteDevice;

    pub fn scan_enabled() -> Predicate<CentralState> {
        Predicate::new(
            |state: &CentralState| -> bool {
                !state.scan_state_changes.is_empty()
                    && state.scan_state_changes.last() == Some(&ScanStateChange::ScanEnabled)
            },
            Some("Scan was enabled"),
        )
    }
    pub fn scan_disabled() -> Predicate<CentralState> {
        Predicate::new(
            |state: &CentralState| -> bool {
                !state.scan_state_changes.is_empty()
                    && state.scan_state_changes.last() == Some(&ScanStateChange::ScanDisabled)
            },
            Some("Scan was disabled"),
        )
    }
    pub fn device_found(expected_name: &str) -> Predicate<CentralState> {
        let expected_name = expected_name.to_string();
        let msg = format!("Peer '{}' has been discovered", expected_name);
        let has_expected_name = move |peer: &RemoteDevice| -> bool {
            peer.advertising_data
                .as_ref()
                .and_then(|ad| ad.name.as_ref())
                .iter()
                .any(|&name| name == &expected_name)
        };

        Predicate::new(
            move |state: &CentralState| -> bool {
                !state.remote_devices.is_empty()
                    && state.remote_devices.iter().any(&has_expected_name)
            },
            Some(&msg),
        )
    }
}

fn scan_timeout() -> Duration {
    10.seconds()
}

async fn add_fake_le_peer(proxy: &HciEmulatorProxy, address: &Address) -> Result<PeerProxy, Error> {
    let (local, remote) = fidl::endpoints::create_proxy()?;
    let params = LowEnergyPeerParameters {
        address: Some(address.clone().into()),
        connectable: Some(true),
        advertisement: Some(AdvertisingData {
            data: vec![
                0x02, 0x01, 0x02, // Flags field set to "general discoverable"
                0x05, 0x09, 'F' as u8, 'a' as u8, 'k' as u8,
                'e' as u8, // Complete local name set to "Fake"
            ],
        }),
        scan_response: None,
    };
    let _ = proxy
        .add_low_energy_peer(params, remote)
        .await?
        .map_err(|e| format_err!("Failed to register fake peer: {:?}", e))?;
    Ok(local)
}

async fn start_scan(central: &CentralHarness) -> Result<(), Error> {
    let status = central
        .aux()
        .proxy()
        .start_scan(None)
        .map_err(|e| e.context("FIDL error sending command").into())
        .on_timeout(scan_timeout().after_now(), move || Err(err_msg("Timed out")))
        .await
        .context("Could not initialize scan")?;
    if let Some(e) = status.error {
        return Err(BTError::from(*e).into());
    }
    Ok(())
}

async fn test_enable_scan(central: CentralHarness) -> Result<(), Error> {
    let emulator = central.aux().emulator().clone();
    let address = Address::Random([1, 0, 0, 0, 0, 0]);
    let _peer = add_fake_le_peer(&emulator, &address).await?;
    start_scan(&central).await?;
    let _ = central
        .when_satisfied(
            central_expectation::scan_enabled().and(central_expectation::device_found("Fake")),
            scan_timeout(),
        )
        .await?;
    Ok(())
}

async fn test_enable_and_disable_scan(central: CentralHarness) -> Result<(), Error> {
    start_scan(&central).await?;
    let _ = central.when_satisfied(central_expectation::scan_enabled(), scan_timeout()).await?;
    let _ = central.aux().proxy().stop_scan()?;
    let _ = central.when_satisfied(central_expectation::scan_disabled(), scan_timeout()).await?;
    Ok(())
}

/// Run all test cases.
pub fn run_all() -> Result<(), Error> {
    run_suite!("le.Central", [test_enable_scan, test_enable_and_disable_scan])
}
