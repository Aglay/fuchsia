// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    failure::bail,
    fidl_fuchsia_paver::{Configuration, PaverMarker},
    fuchsia_component::client::connect_to_service,
    fuchsia_syslog::{fx_log_err, fx_log_info},
    fuchsia_zircon::Status,
};

/// Inform the Paver service that Fuchsia booted successfully, so it marks the partition healthy
/// and stops decrementing the boot counter.
pub async fn set_active_configuration_healthy() {
    if let Err(err) = set_active_configuration_healthy_impl().await {
        fx_log_err!("error marking active configuration successful: {}", err);
    }
}

async fn set_active_configuration_healthy_impl() -> Result<(), failure::Error> {
    let paver = connect_to_service::<PaverMarker>()?;
    match Status::ok(paver.set_active_configuration_healthy().await?) {
        Ok(()) => (),
        Err(Status::NOT_SUPPORTED) => {
            fx_log_info!("ABR not supported");
            return Ok(());
        }
        Err(status) => {
            bail!("set_active_configuration_healthy failed with status {:?}", status);
        }
    };

    // Find out the inactive partition and mark it as unbootable.
    // Note: at this point, we know that ABR is supported.
    let active_config =
        paver.query_active_configuration().await?.map_err(|status| Status::from_raw(status))?;
    let inactive_config = match active_config {
        Configuration::A => Configuration::B,
        Configuration::B => Configuration::A,
        Configuration::Recovery => bail!("Recovery should not be active"),
    };
    Status::ok(paver.set_configuration_unbootable(inactive_config).await?)?;
    Ok(())
}
