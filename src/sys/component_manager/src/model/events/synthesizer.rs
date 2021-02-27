// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::model::{
        component::{ComponentInstance, InstanceState},
        error::ModelError,
        events::{dispatcher::EventDispatcherScope, event::Event, filter::EventFilter},
        hooks::{Event as HookEvent, EventType},
        model::Model,
    },
    async_trait::async_trait,
    cm_rust::CapabilityName,
    fuchsia_async as fasync,
    futures::{channel::mpsc, future::join_all, stream, SinkExt, StreamExt},
    log::error,
    moniker::AbsoluteMoniker,
    std::{
        collections::{HashMap, HashSet},
        sync::{Arc, Weak},
    },
};

/// Implementors of this trait know how to synthesize an event.
#[async_trait]
pub trait EventSynthesisProvider: Send + Sync {
    /// Provides a synthesized event applying the given `filter` under the given `component`.
    async fn provide(
        &self,
        component: Arc<ComponentInstance>,
        filter: EventFilter,
    ) -> Vec<HookEvent>;
}

/// Synthesis manager.
pub struct EventSynthesizer {
    /// A reference to the model.
    model: Weak<Model>,

    /// Maps an event name to the provider for synthesis
    providers: HashMap<CapabilityName, Arc<dyn EventSynthesisProvider>>,
}

impl EventSynthesizer {
    /// Creates a new event synthesizer.
    pub fn new(model: Weak<Model>) -> Self {
        Self { model, providers: HashMap::new() }
    }

    /// Registers a new provider that will be used when synthesizing events of the type `event`.
    pub fn register_provider(
        &mut self,
        event: EventType,
        provider: Arc<dyn EventSynthesisProvider>,
    ) {
        self.providers.insert(CapabilityName(event.to_string()), provider);
    }

    /// Spawns a synthesis task for the requested `events`. Resulting events will be sent on the
    /// `sender` channel.
    pub fn spawn_synthesis(
        &self,
        sender: mpsc::UnboundedSender<Event>,
        events: HashMap<CapabilityName, Vec<EventDispatcherScope>>,
    ) {
        SynthesisTask::new(&self, sender, events).spawn()
    }
}

/// Information about an event that will be synthesized.
struct EventSynthesisInfo {
    /// The provider of the synthesized event.
    provider: Arc<dyn EventSynthesisProvider>,

    /// The scopes under which the event will be synthesized.
    scopes: Vec<EventDispatcherScope>,
}

struct SynthesisTask {
    /// A reference to the model.
    model: Weak<Model>,

    /// The sender end of the channel where synthesized events will be sent.
    sender: mpsc::UnboundedSender<Event>,

    /// Information about the events to synthesize
    event_infos: Vec<EventSynthesisInfo>,
}

impl SynthesisTask {
    /// Creates a new synthesis task from the given events. It will ignore events for which the
    /// `synthesizer` doesn't have a provider.
    pub fn new(
        synthesizer: &EventSynthesizer,
        sender: mpsc::UnboundedSender<Event>,
        mut events: HashMap<CapabilityName, Vec<EventDispatcherScope>>,
    ) -> Self {
        let event_infos = synthesizer
            .providers
            .iter()
            .filter_map(|(event_name, provider)| {
                events
                    .remove(event_name)
                    .map(|scopes| EventSynthesisInfo { provider: provider.clone(), scopes })
            })
            .collect();
        Self { model: synthesizer.model.clone(), sender, event_infos }
    }

    /// Spawns a task that will synthesize all events that were requested when creating the
    /// `SynthesisTask`
    pub fn spawn(self) {
        if self.event_infos.is_empty() {
            return;
        }
        fasync::Task::spawn(async move {
            // If we can't find the component then we can't synthesize events.
            // This isn't necessarily an error as the model or component might've been
            // destroyed in the intervening time, so we just exit early.
            if let Some(model) = self.model.upgrade() {
                let sender = self.sender;
                let futs = self
                    .event_infos
                    .into_iter()
                    .map(|event_info| Self::run(&model, sender.clone(), event_info));
                for result in join_all(futs).await {
                    if let Err(e) = result {
                        error!("Event synthesis failed: {:?}", e);
                    }
                }
            }
        })
        .detach();
    }

    /// Performs a depth-first traversal of the component instance tree. It adds to the stream a
    /// `Running` event for all components that are running. In the case of overlapping scopes,
    /// events are deduped.  It also synthesizes events that were requested which are synthesizable
    /// (there's a provider for them). Those events will only be synthesized if their scope is
    /// within the scope of a Running scope.
    async fn run(
        model: &Arc<Model>,
        mut sender: mpsc::UnboundedSender<Event>,
        info: EventSynthesisInfo,
    ) -> Result<(), ModelError> {
        let mut visited_components = HashSet::new();
        for scope in info.scopes {
            let root = model.look_up(&scope.moniker).await?;
            let mut component_stream = get_subcomponents(root, visited_components.clone());
            while let Some(component) = component_stream.next().await {
                visited_components.insert(component.abs_moniker.clone());
                let events = info.provider.provide(component, scope.filter.clone()).await;
                for event in events {
                    let event =
                        Event { event, scope_moniker: scope.moniker.clone(), responder: None };
                    // Ignore this error. This can occur when the event stream is closed in the
                    // middle of synthesis. We can finish synthesizing if an error happens.
                    if let Err(_) = sender.send(event).await {
                        return Ok(());
                    }
                }
            }
        }
        Ok(())
    }
}

/// Returns all components that are under the given `root` component. Skips the ones whose moniker
/// is contained in the `visited` set.  The visited set is included for early pruning of a tree
/// branch.
fn get_subcomponents(
    root: Arc<ComponentInstance>,
    visited: HashSet<AbsoluteMoniker>,
) -> stream::BoxStream<'static, Arc<ComponentInstance>> {
    let pending = vec![root];
    stream::unfold((pending, visited), move |(mut pending, mut visited)| async move {
        loop {
            match pending.pop() {
                None => return None,
                Some(curr_component) => {
                    if visited.contains(&curr_component.abs_moniker) {
                        continue;
                    }
                    let state_guard = curr_component.lock_state().await;
                    match *state_guard {
                        InstanceState::New
                        | InstanceState::Discovered
                        | InstanceState::Destroyed => {}
                        InstanceState::Resolved(ref s) => {
                            for (_, child) in s.live_children() {
                                pending.push(child.clone());
                            }
                        }
                    }
                    drop(state_guard);
                    visited.insert(curr_component.abs_moniker.clone());
                    return Some((curr_component, (pending, visited)));
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
                dispatcher::EventDispatcherScope,
                event::EventMode,
                filter::EventFilter,
                mode_set::EventModeSet,
                registry::{EventRegistry, RoutedEvent, SubscriptionOptions},
                stream::EventStream,
            },
            hooks::{EventError, EventErrorPayload, EventPayload, EventType},
            testing::{routing_test_helpers::*, test_helpers::*},
        },
        cm_rust::{DirectoryDecl, ExposeDecl, ExposeDirectoryDecl, ExposeSource, ExposeTarget},
        fidl_fuchsia_io2 as fio,
        moniker::AbsoluteMoniker,
        std::{collections::HashSet, iter::FromIterator},
    };

    // Shows that we see Running only for components that are bound at the moment of subscription.
    #[fuchsia::test]
    async fn synthesize_only_running() {
        let test = setup_synthesis_test().await;

        // Bind: b, c, e. We should see Running events only for these.
        test.bind_instance(&vec!["b:0"].into()).await.expect("bind instance b success");
        test.bind_instance(&vec!["c:0"].into()).await.expect("bind instance c success");
        test.bind_instance(&vec!["c:0", "e:0"].into()).await.expect("bind instance e success");

        let registry = test.builtin_environment.event_registry.clone();
        let mut event_stream = create_stream(
            &registry,
            vec![AbsoluteMoniker::root()],
            vec![EventType::Started, EventType::Running],
        )
        .await;

        // Bind f, this will be a Started event.
        test.bind_instance(&vec!["c:0", "f:0"].into()).await.expect("bind instance success");

        let mut result_monikers = HashSet::new();
        while result_monikers.len() < 5 {
            let event = event_stream.next().await.expect("got running event");
            match event.event.result {
                Ok(EventPayload::Running { .. }) => {
                    if event.event.target_moniker.to_string() == "/c:0/f:0" {
                        // There's a chance of receiving Started and Running for the instance we
                        // just bound if it happens to start while we are synthesizing. We assert
                        // that instance separately and count it once.
                        continue;
                    }
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
        let expected_monikers = vec!["/", "/b:0", "/c:0", "/c:0/e:0", "/c:0/f:0"];
        let mut result_monikers = Vec::from_iter(result_monikers.into_iter());
        result_monikers.sort();
        assert_eq!(expected_monikers, result_monikers);
    }

    // Shows that we see Running a single time even if the subscription scopes intersect.
    #[fuchsia::test]
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
        let registry = test.builtin_environment.event_registry.clone();
        let mut event_stream = create_stream(
            &registry,
            vec![vec!["c:0"].into(), vec!["c:0", "e:0"].into(), vec!["c:0", "e:0", "h:0"].into()],
            vec![EventType::Started, EventType::Running],
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
    #[fuchsia::test]
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
        let registry = test.builtin_environment.event_registry.clone();
        let mut event_stream = create_stream(
            &registry,
            vec![vec!["c:0"].into(), vec!["c:0", "e:0"].into(), vec!["c:0", "f:0", "i:0"].into()],
            vec![EventType::Started, EventType::Running],
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

    #[fuchsia::test]
    async fn synthesize_capability_ready() {
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
        test.bind_instance(&vec!["c:0", "f:0", "i:0"].into())
            .await
            .expect("bind instance g success");

        let registry = test.builtin_environment.event_registry.clone();
        // TODO: bind components
        let mut event_stream = create_stream(
            &registry,
            vec![vec!["b:0"].into(), vec!["c:0", "e:0"].into()],
            vec![EventType::Running, EventType::CapabilityReady],
        )
        .await;

        // We expect 4 CapabilityReady events and 5 running events.
        // CR: b, d, e, g
        // RN: b, d, e, g, h
        let expected_capability_ready_monikers =
            vec!["/b:0", "/b:0/d:0", "/c:0/e:0", "/c:0/e:0/g:0"];
        let mut expected_running_monikers = expected_capability_ready_monikers.clone();
        expected_running_monikers.extend(vec!["/c:0/e:0/h:0"].into_iter());
        // We use sets given that the CapabilityReady could be dispatched twice: regular +
        // synthesized.
        let mut result_running_monikers = HashSet::new();
        let mut result_capability_ready_monikers = HashSet::new();
        while result_running_monikers.len() < 5 || result_capability_ready_monikers.len() < 4 {
            let event = event_stream.next().await.expect("got running event");
            match event.event.result {
                Ok(EventPayload::Running { .. }) => {
                    result_running_monikers.insert(event.event.target_moniker.to_string());
                }
                // We get an error cuz the component is not really serving the directory, but is
                // exposing it. For the purposes of the test, this is enough information.
                Err(EventError {
                    event_error_payload: EventErrorPayload::CapabilityReady { name, .. },
                    ..
                }) if name == "diagnostics" => {
                    result_capability_ready_monikers.insert(event.event.target_moniker.to_string());
                }
                payload => panic!("Expected running or capability ready. Got: {:?}", payload),
            }
        }
        let mut result_running = result_running_monikers.into_iter().collect::<Vec<_>>();
        let mut result_capability_ready =
            result_capability_ready_monikers.into_iter().collect::<Vec<_>>();
        result_running.sort();
        result_capability_ready.sort();
        assert_eq!(result_running, expected_running_monikers);
        assert_eq!(result_capability_ready, expected_capability_ready_monikers);
    }

    async fn create_stream(
        registry: &EventRegistry,
        scope_monikers: Vec<AbsoluteMoniker>,
        events: Vec<EventType>,
    ) -> EventStream {
        let scopes = scope_monikers
            .into_iter()
            .map(|moniker| EventDispatcherScope {
                moniker,
                filter: EventFilter::debug(),
                mode_set: EventModeSet::new(cm_rust::EventMode::Sync),
            })
            .collect::<Vec<_>>();
        let events = events
            .into_iter()
            .map(|event| RoutedEvent {
                source_name: event.into(),
                mode: EventMode::Async,
                scopes: scopes.clone(),
            })
            .collect();
        registry
            .subscribe_with_routed_events(&SubscriptionOptions::default(), events)
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
                    .directory(diagnostics_decl())
                    .expose(expose_diagnostics_decl())
                    .add_lazy_child("b")
                    .add_lazy_child("c")
                    .build(),
            ),
            (
                "b",
                ComponentDeclBuilder::new()
                    .directory(diagnostics_decl())
                    .expose(expose_diagnostics_decl())
                    .add_lazy_child("d")
                    .build(),
            ),
            ("c", ComponentDeclBuilder::new().add_lazy_child("e").add_lazy_child("f").build()),
            (
                "d",
                ComponentDeclBuilder::new()
                    .directory(diagnostics_decl())
                    .expose(expose_diagnostics_decl())
                    .build(),
            ),
            (
                "e",
                ComponentDeclBuilder::new()
                    .directory(diagnostics_decl())
                    .expose(expose_diagnostics_decl())
                    .add_lazy_child("g")
                    .add_lazy_child("h")
                    .build(),
            ),
            ("f", ComponentDeclBuilder::new().add_lazy_child("i").build()),
            (
                "g",
                ComponentDeclBuilder::new()
                    .directory(diagnostics_decl())
                    .expose(expose_diagnostics_decl())
                    .build(),
            ),
            ("h", ComponentDeclBuilder::new().build()),
            (
                "i",
                ComponentDeclBuilder::new()
                    .directory(diagnostics_decl())
                    .expose(expose_diagnostics_decl())
                    .build(),
            ),
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
                Ok(EventPayload::Running { .. }) => {
                    result_monikers.push(event.event.target_moniker.to_string());
                }
                payload => panic!("Expected running. Got: {:?}", payload),
            }
        }
        result_monikers.sort();
        result_monikers
    }

    fn diagnostics_decl() -> DirectoryDecl {
        DirectoryDeclBuilder::new("diagnostics").path("/diagnostics").build()
    }

    fn expose_diagnostics_decl() -> ExposeDecl {
        ExposeDecl::Directory(ExposeDirectoryDecl {
            source: ExposeSource::Self_,
            source_name: "diagnostics".into(),
            target_name: "diagnostics".into(),
            target: ExposeTarget::Framework,
            rights: Some(fio::Operations::Connect),
            subdir: None,
        })
    }
}
