// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {component_side_testing::*, failure::Error, fuchsia_async as fasync};

#[fasync::run_singlethreaded]
async fn main() -> Result<(), Error> {
    let testing = ComponentSideTesting::new()?;

    // Read the children of this component and pass the results to the integration test
    // via HubReport.
    testing.report_directory_contents("/hub/children").await?;

    // Read the hub of the child and pass the results to the integration test
    // via HubReport
    testing.report_directory_contents("/hub/children/child").await?;

    // Read the grandchildren and pass the results to the integration test
    // via HubReport
    testing.report_directory_contents("/hub/children/child/children").await?;

    Ok(())
}
