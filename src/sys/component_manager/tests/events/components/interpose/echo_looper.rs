// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::format_err, fidl_fidl_examples_routing_echo as fecho, fuchsia_async as fasync,
    fuchsia_component::client::connect_to_service, fuchsia_syslog as syslog, log::*,
};

#[fasync::run_singlethreaded]
async fn main() {
    syslog::init_with_tags(&["echo_looper"]).expect("failed to init logs");
    let echo = connect_to_service::<fecho::EchoMarker>().expect("error connecting to echo");

    for _ in 1..=10 {
        let out = echo.echo_string(Some("Hippos rule!")).await.expect("echo_string failed");
        let out = out.ok_or(format_err!("empty result")).expect("echo_string got empty result");
        info!("Sent \"Hippos rule!\". Received \"{}\"", out);
    }
}
