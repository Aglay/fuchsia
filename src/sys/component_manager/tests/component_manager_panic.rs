// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {anyhow::Error, fuchsia_async as fasync, test_utils_lib::test_utils::BlackBoxTest};

#[fasync::run_singlethreaded(test)]
async fn test() -> Result<(), Error> {
    // For the root component manifest, pass in the path the component manager
    // itself, which should be completely invalid.
    let mut test =
        BlackBoxTest::default("fuchsia-pkg://fuchsia.com/component_manager#bin/component_manager")
            .await?;

    // We expect that component manager will crash since the root component
    // manifest is invalid.
    let exit_status = test.component_manager_app.wait().await?;
    if exit_status.success() {
        panic!("component manager should have exited with error, but exited normally");
    }
    Ok(())
}
