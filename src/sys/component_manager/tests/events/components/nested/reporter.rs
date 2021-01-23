// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    fidl_fidl_examples_routing_echo as fecho, fuchsia_async as fasync,
    fuchsia_component::client::connect_to_service,
    fuchsia_syslog as syslog,
    test_utils_lib::{
        events::{Event, EventMode, EventSource, EventSubscription, Handler, Started},
        matcher::EventMatcher,
    },
};

#[fasync::run_singlethreaded]
async fn main() {
    syslog::init_with_tags(&["nested_reporter"]).unwrap();

    // Track all the starting child components.
    let event_source = EventSource::new_async().unwrap();
    let mut event_stream = event_source
        .subscribe(vec![EventSubscription::new(vec![Started::NAME], EventMode::Sync)])
        .await
        .unwrap();
    event_source.drop_event_stream("/svc/StartComponentTree").await;

    let echo = connect_to_service::<fecho::EchoMarker>().unwrap();

    for _ in 1..=3 {
        let event = EventMatcher::ok().expect_match::<Started>(&mut event_stream).await;
        let target_moniker = event.target_moniker();
        let _ = echo.echo_string(Some(target_moniker)).await;
        event.resume().await.unwrap();
    }
}
