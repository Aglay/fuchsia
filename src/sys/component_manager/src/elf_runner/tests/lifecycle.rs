// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    fuchsia_async as fasync,
    fuchsia_component::client::ScopedInstance,
    fuchsia_syslog::{self as fxlog},
    test_utils_lib::events::{Event, EventMatcher, EventSource, Ordering, Stopped},
};

#[fasync::run_singlethreaded(test)]
async fn test_normal_behavior() {
    fxlog::init().unwrap();

    let event_source = EventSource::new_sync().unwrap();
    let mut event_stream = event_source.subscribe(vec![Stopped::NAME]).await.unwrap();
    event_source.start_component_tree().await.unwrap();
    let collection_name = String::from("test-collection");
    // What is going on here? A scoped dynamic instance is created and then
    // dropped. When a the instance is dropped it stops the instance.
    let (child_name, destroy_waiter) = {
        let mut instance = ScopedInstance::new(
            collection_name.clone(),
            String::from(
                "fuchsia-pkg://fuchsia.com/elf_runner_lifecycle_test#meta/lifecycle-full.cm",
            ),
        )
        .await
        .unwrap();

        (instance.child_name(), instance.take_destroy_waiter())
    };
    if let Some(e) = destroy_waiter.await {
        panic!("failed to destroy child: {:?}", e);
    }

    // TODO(47324) Once the runner watches for abnormal exit behavior, validate
    // that the component under test exited normally.
    let expected_events = vec![EventMatcher::new()
        .expect_type::<Stopped>()
        .expect_moniker(format!("./{}:{}:*", collection_name, child_name))];
    event_stream.validate(Ordering::Ordered, expected_events).await.unwrap();
}
