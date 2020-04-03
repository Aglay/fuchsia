// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::Error,
    echo_factory_interposer::EchoFactoryInterposer,
    echo_interposer::EchoInterposer,
    fuchsia_async as fasync,
    futures::StreamExt,
    test_utils_lib::{echo_capability::EchoCapability, events::*, test_utils::*},
};

#[fasync::run_singlethreaded(test)]
async fn async_event_source_test() -> Result<(), Error> {
    let test = BlackBoxTest::default(
        "fuchsia-pkg://fuchsia.com/events_integration_test#meta/async_reporter.cm",
    )
    .await?;

    let event_source = test.connect_to_event_source().await?;

    let (capability, mut echo_rx) = EchoCapability::new();
    let injector = event_source.install_injector(capability).await?;

    event_source.start_component_tree().await?;

    let mut events = vec![];
    for _ in 1..=6 {
        let event = echo_rx.next().await.unwrap();
        events.push(event.message.clone());
        event.resume();
    }
    assert_eq!(
        vec!["Started", "Started", "Started", "Destroyed", "Destroyed", "Destroyed"],
        events
    );
    injector.abort();

    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn echo_interposer_test() -> Result<(), Error> {
    let test = BlackBoxTest::default(
        "fuchsia-pkg://fuchsia.com/events_integration_test#meta/interpose_echo_realm.cm",
    )
    .await?;

    let event_source = test.connect_to_event_source().await?;

    // Setup the interposer
    let (echo_interposer, mut rx) = EchoInterposer::new();
    let interposer = event_source.install_interposer(echo_interposer).await?;

    event_source.start_component_tree().await?;

    // Ensure that the string "Interposed: Hippos rule!" is sent 10 times as a response
    // from server to client.
    for _ in 1..=10 {
        let echo_string = rx.next().await.expect("local tx/rx channel was closed");
        assert_eq!(echo_string, "Interposed: Hippos rule!");
    }
    interposer.abort();

    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn scoped_events_test() -> Result<(), Error> {
    let test = BlackBoxTest::default(
        "fuchsia-pkg://fuchsia.com/events_integration_test#meta/echo_realm.cm",
    )
    .await?;

    let event_source = test.connect_to_event_source().await?;

    // Inject an echo capability for `echo_reporter` so that we can observe its messages here.
    let mut echo_rx = {
        let mut event_stream = event_source.subscribe(vec![CapabilityRouted::NAME]).await?;

        event_source.start_component_tree().await?;

        // Wait for `echo_reporter` to attempt to connect to the Echo service
        let event = event_stream
            .wait_until_framework_capability(
                "./echo_reporter:0",
                "/svc/fidl.examples.routing.echo.Echo",
                Some("./echo_reporter:0"),
            )
            .await?;

        // Setup the echo capability.
        let (capability, echo_rx) = EchoCapability::new();
        event.inject(capability).await?;
        event.resume().await?;

        echo_rx
    };

    // Wait to receive the start trigger that echo_reporter recieved. This
    // indicates to `echo_reporter` that it should start collecting `CapabilityRouted`
    // events.
    let start_trigger_echo = echo_rx.next().await.unwrap();
    assert_eq!(start_trigger_echo.message, "Start trigger");
    start_trigger_echo.resume();

    // This indicates that `echo_reporter` will stop receiving `CapabilityRouted`
    // events.
    let stop_trigger_echo = echo_rx.next().await.unwrap();
    assert_eq!(stop_trigger_echo.message, "Stop trigger");
    stop_trigger_echo.resume();

    // Verify that the `echo_reporter` sees `Started` and
    // a `CapabilityRouted` event to itself (routing the ELF runner
    // capability at startup), but not other `CapabilityRouted` events
    // because the target of other `CapabilityRouted` events are outside
    // its realm.
    let events_echo = echo_rx.next().await.unwrap();
    assert_eq!(
        events_echo.message,
        concat!(
            "Events: [",
            "RecordedEvent { event_type: CapabilityRouted, target_moniker: \"./echo_server:0\", capability_id: Some(\"elf\") }, ",
            "RecordedEvent { event_type: Started, target_moniker: \"./echo_server:0\", capability_id: None }",
            "]"
        )
    );
    events_echo.resume();

    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn realm_offered_event_source_test() -> Result<(), Error> {
    let test = BlackBoxTest::default(
        "fuchsia-pkg://fuchsia.com/events_integration_test#meta/realm_offered_root.cm",
    )
    .await?;

    let event_source = test.connect_to_event_source().await?;

    // Inject echo capability for `root/nested_realm/reporter` so that we can observe its messages
    // here.
    let mut echo_rx = {
        let mut event_stream = event_source.subscribe(vec![CapabilityRouted::NAME]).await?;

        event_source.start_component_tree().await?;

        // Wait for `reporter` to connect to the service.
        let event = event_stream
            .wait_until_framework_capability(
                "./nested_realm:0/reporter:0",
                "/svc/fidl.examples.routing.echo.Echo",
                Some("./nested_realm:0/reporter:0"),
            )
            .await?;

        // Setup the echo capability.
        let (capability, echo_rx) = EchoCapability::new();
        event.inject(capability).await?;
        event.resume().await?;

        echo_rx
    };

    // Verify that the `reporter` sees `Started` for the three components started under the
    // `nested_realm`.
    for child in vec!["a", "b", "c"] {
        let events_echo = echo_rx.next().await.unwrap();
        assert_eq!(events_echo.message, format!("./child_{}:0", child));
        events_echo.resume();
    }

    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn nested_event_source_test() -> Result<(), Error> {
    let test = BlackBoxTest::default(
        "fuchsia-pkg://fuchsia.com/events_integration_test#meta/nested_reporter.cm",
    )
    .await?;

    let event_source = test.connect_to_event_source().await?;

    let (capability, mut echo_rx) = EchoCapability::new();
    let injector = event_source.install_injector(capability).await?;

    event_source.start_component_tree().await?;

    let mut children = vec![];
    for _ in 1..=3 {
        let child = echo_rx.next().await.unwrap();
        println!("child: {}", child.message);
        children.push(child.message.clone());
        child.resume();
    }
    children.sort_unstable();
    assert_eq!(vec!["./child_a:0", "./child_b:0", "./child_c:0"], children);
    injector.abort();

    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn chained_interposer_test() -> Result<(), Error> {
    let test = BlackBoxTest::default(
        "fuchsia-pkg://fuchsia.com/events_integration_test#meta/chained_interpose_echo_realm.cm",
    )
    .await?;

    let event_source = test.connect_to_event_source().await?;

    let (capability, mut echo_rx) = EchoFactoryInterposer::new();
    let injector = event_source.install_interposer(capability).await?;

    event_source.start_component_tree().await?;

    let mut messages = vec![];
    for _ in 1..=3 {
        let message = echo_rx.next().await.unwrap();
        messages.push(message.clone());
    }
    messages.sort_unstable();
    assert_eq!(vec!["Interposed: a", "Interposed: b", "Interposed: c"], messages);
    injector.abort();

    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn event_dispatch_order_test() -> Result<(), Error> {
    let test = BlackBoxTest::default(
        "fuchsia-pkg://fuchsia.com/events_integration_test#meta/event_dispatch_order_root.cm",
    )
    .await?;

    let event_source = test.connect_to_event_source().await?;
    let mut event_stream = event_source.subscribe(vec![Discovered::NAME, Resolved::NAME]).await?;

    event_source.start_component_tree().await?;

    // "Discovered" is the first stage of a component's lifecycle so it must
    // be dispatched before "Resolved". Also, a child is not discovered until
    // the parent is resolved and its manifest is processed.
    event_stream.expect_exact::<Discovered>(".").await?.resume().await?;
    event_stream.expect_exact::<Resolved>(".").await?.resume().await?;
    event_stream.expect_exact::<Discovered>("./child:0").await?.resume().await?;
    event_stream.expect_exact::<Resolved>("./child:0").await?.resume().await?;

    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn event_capability_ready() -> Result<(), Error> {
    let test = BlackBoxTest::default(
        "fuchsia-pkg://fuchsia.com/events_integration_test#meta/capability_ready_root.cm",
    )
    .await?;

    let event_source = test.connect_to_event_source().await?;

    let (capability, mut echo_rx) = EchoCapability::new();
    let injector = event_source.install_injector(capability).await?;

    event_source.start_component_tree().await?;

    let mut messages = vec![];
    for _ in 0..2 {
        let event = echo_rx.next().await.unwrap();
        messages.push(event.message.clone());
        event.resume();
    }
    messages.sort_unstable();
    assert_eq!(vec!["Saw /bar on ./child:0", "Saw /foo on ./child:0",], messages);
    injector.abort();

    Ok(())
}
