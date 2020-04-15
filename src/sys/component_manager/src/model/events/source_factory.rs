// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        capability::{CapabilityProvider, CapabilitySource, FrameworkCapability},
        model::{
            error::ModelError,
            events::{event::SyncMode, registry::EventRegistry, source::EventSource},
            hooks::{Event, EventPayload, EventType, Hook, HooksRegistration},
            model::Model,
            moniker::AbsoluteMoniker,
        },
    },
    async_trait::async_trait,
    cm_rust::{CapabilityPath, ComponentDecl},
    futures::lock::Mutex,
    lazy_static::lazy_static,
    std::{
        collections::HashMap,
        convert::TryInto,
        sync::{Arc, Weak},
    },
};

lazy_static! {
    pub static ref EVENT_SOURCE_SERVICE_PATH: CapabilityPath =
        "/svc/fuchsia.sys2.EventSource".try_into().unwrap();
    pub static ref EVENT_SOURCE_SYNC_SERVICE_PATH: CapabilityPath =
        "/svc/fuchsia.sys2.BlockingEventSource".try_into().unwrap();
}

/// Allows to create `EventSource`s and tracks all the created ones.
pub struct EventSourceFactory {
    /// Tracks the event source used by each component ideantified with the given `moniker`.
    event_source_registry: Mutex<HashMap<AbsoluteMoniker, EventSource>>,

    /// The event registry. It subscribes to all events happening in the system and
    /// routes them to subscribers.
    // TODO(fxb/48512): instead of using a global registry integrate more with the hooks model.
    event_registry: Arc<EventRegistry>,

    /// The component model, needed to route events.
    model: Weak<Model>,
}

impl EventSourceFactory {
    /// Creates a new event source factory.
    pub fn new(model: Weak<Model>) -> Self {
        Self {
            event_source_registry: Mutex::new(HashMap::new()),
            event_registry: Arc::new(EventRegistry::new()),
            model,
        }
    }

    /// Creates the subscription to the required events.
    /// `CapabilityReady` used to track events and associate them with the component that needs them
    /// as well as the scoped that will be allowed. Also the EventSource protocol capability.
    pub fn hooks(self: &Arc<Self>) -> Vec<HooksRegistration> {
        let mut hooks = self.event_registry.hooks();
        hooks.append(&mut vec![
            // This hook provides the EventSource capability to components in the tree
            HooksRegistration::new(
                "EventSourceFactory",
                vec![EventType::CapabilityRouted, EventType::Destroyed, EventType::Resolved],
                Arc::downgrade(self) as Weak<dyn Hook>,
            ),
        ]);
        hooks
    }

    /// Creates a debug event source.
    pub async fn create_for_debug(&self, sync_mode: SyncMode) -> Result<EventSource, ModelError> {
        EventSource::new_for_debug(
            self.model.clone(),
            AbsoluteMoniker::root(),
            &self.event_registry,
            sync_mode,
        )
        .await
    }

    /// Creates a `EventSource` for the given `target_moniker`.
    pub async fn create(
        &self,
        target_moniker: AbsoluteMoniker,
        sync_mode: SyncMode,
    ) -> Result<EventSource, ModelError> {
        EventSource::new(self.model.clone(), target_moniker, &self.event_registry, sync_mode).await
    }

    /// Returns an EventSource. An EventSource holds an AbsoluteMoniker that
    /// corresponds to the realm in which it will receive events.
    async fn on_scoped_framework_capability_routed_async(
        self: Arc<Self>,
        capability_decl: &FrameworkCapability,
        target_moniker: AbsoluteMoniker,
        _scope_moniker: AbsoluteMoniker,
        capability: Option<Box<dyn CapabilityProvider>>,
    ) -> Result<Option<Box<dyn CapabilityProvider>>, ModelError> {
        match (capability, capability_decl) {
            (None, FrameworkCapability::Protocol(source_path))
                if *source_path == *EVENT_SOURCE_SERVICE_PATH
                    || *source_path == *EVENT_SOURCE_SYNC_SERVICE_PATH =>
            {
                let event_source_registry = self.event_source_registry.lock().await;
                if let Some(event_source) = event_source_registry.get(&target_moniker) {
                    Ok(Some(Box::new(event_source.clone()) as Box<dyn CapabilityProvider>))
                } else {
                    return Err(ModelError::capability_discovery_error(format!(
                        "Unable to find EventSource in registry for {}",
                        target_moniker
                    )));
                }
            }
            (c, _) => return Ok(c),
        }
    }

    async fn on_destroyed_async(self: &Arc<Self>, target_moniker: &AbsoluteMoniker) {
        let mut event_source_registry = self.event_source_registry.lock().await;
        event_source_registry.remove(&target_moniker);
    }

    async fn on_resolved_async(
        self: &Arc<Self>,
        target_moniker: &AbsoluteMoniker,
        decl: &ComponentDecl,
    ) -> Result<(), ModelError> {
        let sync_mode = if decl.uses_protocol_from_framework(&EVENT_SOURCE_SERVICE_PATH) {
            SyncMode::Async
        } else if decl.uses_protocol_from_framework(&EVENT_SOURCE_SYNC_SERVICE_PATH) {
            SyncMode::Sync
        } else {
            return Ok(());
        };
        let key = target_moniker.clone();
        let mut event_source_registry = self.event_source_registry.lock().await;
        // It is currently assumed that a component instance's declaration
        // is resolved only once. Someday, this may no longer be true if individual
        // components can be updated.
        assert!(!event_source_registry.contains_key(&key));
        // An EventSource is created on resolution in order to ensure that discovery
        // and resolution of children is not missed.
        let event_source = self.create(key.clone(), sync_mode).await?;
        event_source_registry.insert(key, event_source);
        Ok(())
    }

    #[cfg(test)]
    async fn has_event_source(&self, abs_moniker: &AbsoluteMoniker) -> bool {
        let event_source_registry = self.event_source_registry.lock().await;
        event_source_registry.contains_key(abs_moniker)
    }
}

#[async_trait]
impl Hook for EventSourceFactory {
    async fn on(self: Arc<Self>, event: &Event) -> Result<(), ModelError> {
        match &event.payload {
            EventPayload::CapabilityRouted {
                source:
                    CapabilitySource::Framework { capability, scope_moniker: Some(scope_moniker) },
                capability_provider,
            } => {
                let mut capability_provider = capability_provider.lock().await;
                *capability_provider = self
                    .on_scoped_framework_capability_routed_async(
                        &capability,
                        event.target_moniker.clone(),
                        scope_moniker.clone(),
                        capability_provider.take(),
                    )
                    .await?;
            }
            EventPayload::Destroyed => {
                self.on_destroyed_async(&event.target_moniker).await;
            }
            EventPayload::Resolved { decl } => {
                self.on_resolved_async(&event.target_moniker, decl).await?;
            }
            _ => {}
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        crate::model::{
            hooks::Hooks, model::ModelParams, resolver::ResolverRegistry,
            testing::test_helpers::ComponentDeclBuilder,
        },
        cm_rust::{UseDecl, UseProtocolDecl, UseSource},
    };

    async fn dispatch_resolved_event(
        hooks: &Hooks,
        target_moniker: &AbsoluteMoniker,
    ) -> Result<(), ModelError> {
        let decl = ComponentDeclBuilder::new()
            .use_(UseDecl::Protocol(UseProtocolDecl {
                source: UseSource::Framework,
                source_path: (*EVENT_SOURCE_SYNC_SERVICE_PATH).clone(),
                target_path: (*EVENT_SOURCE_SYNC_SERVICE_PATH).clone(),
            }))
            .build();
        let event =
            Event::new(target_moniker.clone(), EventPayload::Resolved { decl: decl.clone() });
        hooks.dispatch(&event).await
    }

    async fn dispatch_destroyed_event(
        hooks: &Hooks,
        target_moniker: &AbsoluteMoniker,
    ) -> Result<(), ModelError> {
        let event = Event::new(target_moniker.clone(), EventPayload::Destroyed);
        hooks.dispatch(&event).await
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn drop_event_source_when_component_destroyed() {
        let model = {
            let registry = ResolverRegistry::new();
            Arc::new(Model::new(ModelParams {
                root_component_url: "test:///root".to_string(),
                root_resolver_registry: registry,
            }))
        };
        let event_source_factory = Arc::new(EventSourceFactory::new(Arc::downgrade(&model)));

        let hooks = Hooks::new(None);
        hooks.install(event_source_factory.hooks()).await;

        let root = AbsoluteMoniker::root();

        // Verify that there is no EventSource for the root until we dispatch the Resolved event.
        assert!(!event_source_factory.has_event_source(&root).await);
        dispatch_resolved_event(&hooks, &root).await.unwrap();
        assert!(event_source_factory.has_event_source(&root).await);
        // Verify that destroying the component destroys the EventSource.
        dispatch_destroyed_event(&hooks, &root).await.unwrap();
        assert!(!event_source_factory.has_event_source(&root).await);
    }
}
