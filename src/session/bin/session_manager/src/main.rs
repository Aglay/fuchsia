// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::Error,
    fuchsia_async as fasync,
    session_manager_lib::{service_management, startup},
};

#[fasync::run_singlethreaded]
async fn main() -> Result<(), Error> {
    fuchsia_syslog::init_with_tags(&["session_manager"]).expect("Failed to initialize logger.");

    let mut session_url = String::new();

    if let Some(startup_url) = startup::get_session_url() {
        // Launch the session which was provided to the session manager at startup.
        startup::launch_session(&startup_url).await?;
        session_url = startup_url;
    }

    // Start serving the services exposed by session manager.
    service_management::expose_services(&mut session_url).await?;
    Ok(())
}
