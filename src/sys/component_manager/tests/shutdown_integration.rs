// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {anyhow::Error, fuchsia_async as fasync, test_utils_lib::test_utils::*};

#[fasync::run_singlethreaded(test)]
async fn test() -> Result<(), Error> {
    let mut test = OpaqueTest::default(
        "fuchsia-pkg://fuchsia.com/shutdown_integration_test#meta/shutdown_integration_root.cm",
    )
    .await?;

    test.connect_to_event_source().await?.start_component_tree().await?;

    test.component_manager_app
        .wait()
        .await
        .and_then(|exit_status| exit_status.ok().map_err(|e| e.into()))
}
