// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::model::{
        error::ModelError,
        events::{event::Event, registry::RoutedEvent},
        hooks::{Event as HookEvent, EventPayload},
        model::Model,
        moniker::AbsoluteMoniker,
        realm::Realm,
    },
    fuchsia_async as fasync,
    futures::{channel::mpsc, stream, SinkExt, StreamExt},
    log::error,
    std::{collections::HashSet, sync::Arc, sync::Weak},
};

pub struct EventSynthesizer {
    model: Weak<Model>,
}

impl EventSynthesizer {
    pub fn new(model: Weak<Model>) -> Self {
        Self { model }
    }

    pub fn spawn_synthesis(&self, sender: mpsc::UnboundedSender<Event>, events: Vec<RoutedEvent>) {
        let model = self.model.clone();
        fasync::spawn(async move {
            synthesize(model, sender, events).await.unwrap_or_else(|e| {
                error!("Event synthesis failed: {:?}", e);
            });
        });
    }
}

/// Performs a depth-first traversal of the realm tree. It adds to the stream a `Running` event
/// for all realms that are running. In the case of overlapping scopes, events are deduped.
async fn synthesize(
    model: Weak<Model>,
    mut sender: mpsc::UnboundedSender<Event>,
    events: Vec<RoutedEvent>,
) -> Result<(), ModelError> {
    // TODO: first validate the RoutedEvents to extract the ones we'll synthesize and ensure
    // Running is present.
    let scopes = match events
        .into_iter()
        .find(|event| event.source_name.str() == "running")
        .map(|event| event.scopes)
    {
        None => return Ok(()),
        Some(scopes) => scopes,
    };
    let model = model.upgrade().ok_or(ModelError::ModelNotAvailable)?;

    let mut visited_realms = HashSet::new();

    for scope in scopes {
        let root_realm = model.look_up_realm(&scope.moniker).await?;
        let mut realm_stream = get_subrealms(root_realm, visited_realms.clone());
        while let Some(realm) = realm_stream.next().await {
            let started_time = match &realm.lock_execution().await.runtime {
                Some(runtime) => runtime.timestamp,
                // No runtime means the component is not running.
                None => continue,
            };
            let event = HookEvent::new_with_timestamp(
                realm.abs_moniker.clone(),
                Ok(EventPayload::Running),
                started_time,
            );
            let event = Event { event, scope_moniker: scope.moniker.clone(), responder: None };
            if let Err(_) = sender.send(event).await {
                // Ignore this error. This can occur when the event stream is closed in the middle
                // of synthesis.
                return Ok(());
            }
            visited_realms.insert(realm.abs_moniker.clone());
        }
    }
    Ok(())
}

/// Returns all realms that are under the given `root` realm. Skips the ones whose moniker is
/// contained in the `visited` set.
/// The visited set is included for early pruning of a tree branch.
fn get_subrealms(
    root: Arc<Realm>,
    visited: HashSet<AbsoluteMoniker>,
) -> stream::BoxStream<'static, Arc<Realm>> {
    let pending = vec![root];
    stream::unfold((pending, visited), move |(mut pending, mut visited)| async move {
        loop {
            match pending.pop() {
                None => return None,
                Some(curr_realm) => {
                    if visited.contains(&curr_realm.abs_moniker) {
                        continue;
                    }
                    let state_guard = curr_realm.lock_state().await;
                    if let Some(state) = state_guard.as_ref() {
                        for (_, child_realm) in state.live_child_realms() {
                            pending.push(child_realm.clone());
                        }
                    }
                    drop(state_guard);
                    visited.insert(curr_realm.abs_moniker.clone());
                    return Some((curr_realm, (pending, visited)));
                }
            }
        }
    })
    .boxed()
}

#[cfg(test)]
mod tests {
    use {
        crate::model::{
            events::{
                dispatcher::ScopeMetadata,
                event::SyncMode,
                filter::EventFilter,
                registry::{EventRegistry, RoutedEvent},
                stream::EventStream,
            },
            hooks::{EventPayload, EventType},
            moniker::AbsoluteMoniker,
            testing::{routing_test_helpers::*, test_helpers::*},
        },
        fuchsia_async as fasync,
        std::{collections::HashSet, iter::FromIterator},
    };

    // Shows that we see Running only for realms that are bound at the moment of subscription.
    #[fasync::run_singlethreaded(test)]
    async fn synthesize_only_running() {
        let test = setup_synthesis_test().await;

        // Bind: b, c, d. We should see Running events only for these.
        test.bind_instance(&vec!["b:0"].into()).await.expect("bind instance b success");
        test.bind_instance(&vec!["c:0"].into()).await.expect("bind instance c success");
        test.bind_instance(&vec!["c:0", "e:0"].into()).await.expect("bind instance d success");

        let registry = test.builtin_environment.event_source_factory.registry();
        let mut event_stream = create_stream(&registry, vec![AbsoluteMoniker::root()]).await;

        // Bind e, this will be a Started event.
        test.bind_instance(&vec!["c:0", "f:0"].into()).await.expect("bind instance success");

        let mut result_monikers = HashSet::new();
        for _ in 0..6 {
            // We should see only 5 unique monikers, however there's a chance of receiving Started
            // and Running for the instance we just bound if it happens to start while we are
            // synthesizing. We assert that instance separately and count it once.
            if result_monikers.len() == 4 {
                break;
            }
            let event = event_stream.next().await.expect("got running event");
            match event.event.result {
                Ok(EventPayload::Running) => {
                    result_monikers.insert(event.event.target_moniker.to_string());
                }
                Ok(EventPayload::Started { .. }) => {
                    assert_eq!(event.event.target_moniker.to_string(), "/c:0/f:0");
                    result_monikers.insert(event.event.target_moniker.to_string());
                }
                payload => panic!("Expected running. Got: {:?}", payload),
            }
        }

        // Events might be out of order, sort them
        let expected_monikers = vec!["/", "/b:0", "/c:0", "/c:0/e:0"];
        let mut result_monikers = Vec::from_iter(result_monikers.into_iter());
        result_monikers.sort();
        assert_eq!(expected_monikers, result_monikers);

        let event = event_stream.next().await.expect("got started event");
        match event.event.result {
            Ok(EventPayload::Started { .. }) => {
                assert_eq!("/c:0/f:0", event.event.target_moniker.to_string());
            }
            payload => panic!("Expected started. Got: {:?}", payload),
        }
    }

    // Shows that we see Running a single time even if the subscription scopes intersect.
    #[fasync::run_singlethreaded(test)]
    async fn synthesize_overlapping_scopes() {
        let test = setup_synthesis_test().await;

        test.bind_instance(&vec!["b:0"].into()).await.expect("bind instance b success");
        test.bind_instance(&vec!["c:0"].into()).await.expect("bind instance c success");
        test.bind_instance(&vec!["b:0", "d:0"].into()).await.expect("bind instance d success");
        test.bind_instance(&vec!["c:0", "e:0"].into()).await.expect("bind instance e success");
        test.bind_instance(&vec!["c:0", "e:0", "g:0"].into())
            .await
            .expect("bind instance g success");
        test.bind_instance(&vec!["c:0", "e:0", "h:0"].into())
            .await
            .expect("bind instance h success");
        test.bind_instance(&vec!["c:0", "f:0"].into()).await.expect("bind instance f success");

        // Subscribing with scopes /c, /c/e and /c/e/h
        let registry = test.builtin_environment.event_source_factory.registry();
        let mut event_stream = create_stream(
            &registry,
            vec![vec!["c:0"].into(), vec!["c:0", "e:0"].into(), vec!["c:0", "e:0", "h:0"].into()],
        )
        .await;

        let result_monikers = get_and_sort_running_events(&mut event_stream, 5).await;
        let expected_monikers =
            vec!["/c:0", "/c:0/e:0", "/c:0/e:0/g:0", "/c:0/e:0/h:0", "/c:0/f:0"];
        assert_eq!(expected_monikers, result_monikers);

        // Verify we don't get more Running events.
        test.bind_instance(&vec!["c:0", "f:0", "i:0"].into())
            .await
            .expect("bind instance g success");
        let event = event_stream.next().await.expect("got started event");
        match event.event.result {
            Ok(EventPayload::Started { .. }) => {
                assert_eq!("/c:0/f:0/i:0", event.event.target_moniker.to_string());
            }
            payload => panic!("Expected started. Got: {:?}", payload),
        }
    }

    // Shows that we see Running only for components under the given scopes.
    #[fasync::run_singlethreaded(test)]
    async fn synthesize_non_overlapping_scopes() {
        let test = setup_synthesis_test().await;

        test.bind_instance(&vec!["b:0"].into()).await.expect("bind instance b success");
        test.bind_instance(&vec!["b:0", "d:0"].into()).await.expect("bind instance d success");
        test.bind_instance(&vec!["c:0"].into()).await.expect("bind instance c success");
        test.bind_instance(&vec!["c:0", "e:0"].into()).await.expect("bind instance e success");
        test.bind_instance(&vec!["c:0", "e:0", "g:0"].into())
            .await
            .expect("bind instance g success");
        test.bind_instance(&vec!["c:0", "e:0", "h:0"].into())
            .await
            .expect("bind instance g success");
        test.bind_instance(&vec!["c:0", "f:0"].into()).await.expect("bind instance g success");

        // Subscribing with scopes /c, /c/e and c/f/i
        let registry = test.builtin_environment.event_source_factory.registry();
        let mut event_stream = create_stream(
            &registry,
            vec![vec!["c:0"].into(), vec!["c:0", "e:0"].into(), vec!["c:0", "f:0", "i:0"].into()],
        )
        .await;

        let result_monikers = get_and_sort_running_events(&mut event_stream, 5).await;
        let expected_monikers =
            vec!["/c:0", "/c:0/e:0", "/c:0/e:0/g:0", "/c:0/e:0/h:0", "/c:0/f:0"];
        assert_eq!(expected_monikers, result_monikers);

        // Verify we don't get more Running events.
        test.bind_instance(&vec!["c:0", "f:0", "i:0"].into())
            .await
            .expect("bind instance g success");
        let event = event_stream.next().await.expect("got started event");
        match event.event.result {
            Ok(EventPayload::Started { .. }) => {
                assert_eq!("/c:0/f:0/i:0", event.event.target_moniker.to_string());
            }
            payload => panic!("Expected started. Got: {:?}", payload),
        }
    }

    async fn create_stream(
        registry: &EventRegistry,
        scope_monikers: Vec<AbsoluteMoniker>,
    ) -> EventStream {
        let scopes = scope_monikers
            .into_iter()
            .map(|moniker| ScopeMetadata { moniker, filter: EventFilter::debug() })
            .collect::<Vec<_>>();
        registry
            .subscribe(
                &SyncMode::Async,
                vec![
                    RoutedEvent { source_name: EventType::Running.into(), scopes: scopes.clone() },
                    RoutedEvent { source_name: EventType::Started.into(), scopes },
                ],
            )
            .await
            .expect("subscribe to event stream")
    }

    // Sets up the following topology (all children are lazy)
    //
    //     a
    //    / \
    //   b   c
    //  /   / \
    // d   e   f
    //    / \   \
    //   g   h   i
    async fn setup_synthesis_test() -> RoutingTest {
        let components = vec![
            (
                "a",
                ComponentDeclBuilder::new()
                    .add_lazy_child("b")
                    .add_lazy_child("c")
                    .offer_runner_to_children(TEST_RUNNER_NAME)
                    .build(),
            ),
            (
                "b",
                ComponentDeclBuilder::new()
                    .add_lazy_child("d")
                    .offer_runner_to_children(TEST_RUNNER_NAME)
                    .build(),
            ),
            (
                "c",
                ComponentDeclBuilder::new()
                    .add_lazy_child("e")
                    .add_lazy_child("f")
                    .offer_runner_to_children(TEST_RUNNER_NAME)
                    .build(),
            ),
            ("d", ComponentDeclBuilder::new().build()),
            (
                "e",
                ComponentDeclBuilder::new()
                    .add_lazy_child("g")
                    .add_lazy_child("h")
                    .offer_runner_to_children(TEST_RUNNER_NAME)
                    .build(),
            ),
            (
                "f",
                ComponentDeclBuilder::new()
                    .add_lazy_child("i")
                    .offer_runner_to_children(TEST_RUNNER_NAME)
                    .build(),
            ),
            ("g", ComponentDeclBuilder::new().build()),
            ("h", ComponentDeclBuilder::new().build()),
            ("i", ComponentDeclBuilder::new().build()),
        ];
        RoutingTest::new("a", components).await
    }

    async fn get_and_sort_running_events(
        event_stream: &mut EventStream,
        total: usize,
    ) -> Vec<String> {
        let mut result_monikers = Vec::new();
        for _ in 0..total {
            let event = event_stream.next().await.expect("got running event");
            match event.event.result {
                Ok(EventPayload::Running) => {
                    result_monikers.push(event.event.target_moniker.to_string());
                }
                payload => panic!("Expected running. Got: {:?}", payload),
            }
        }
        result_monikers.sort();
        result_monikers
    }
}
