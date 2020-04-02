// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        capability::{CapabilityProvider, CapabilitySource},
        model::{
            error::ModelError, moniker::AbsoluteMoniker, realm::Runtime,
            routing_facade::RoutingFacade,
        },
    },
    anyhow::format_err,
    async_trait::async_trait,
    cm_rust::ComponentDecl,
    fidl_fuchsia_io::{self as fio, DirectoryProxy, NodeProxy},
    fidl_fuchsia_sys2 as fsys, fuchsia_trace as trace,
    futures::{future::BoxFuture, lock::Mutex},
    io_util,
    rand::random,
    std::{
        collections::HashMap,
        convert::TryFrom,
        fmt,
        sync::{Arc, Weak},
    },
};

/// Defines the `EventType` enum as well as its implementation.
/// |description| is the description of the event that will be a doc comment on that event type.
/// |name| is the name of the event on CamelCase format, capitalized.
/// |string_name| is the name of the event on snake_case format, not capitalized.
macro_rules! events {
    ([$($(#[$description:meta])* ($name:ident, $string_name:ident),)*]) => {
        #[derive(Clone, Debug, Eq, PartialEq, Hash)]
        pub enum EventType {
            $(
                $(#[$description])*
                $name,
            )*
        }

        impl ToString for EventType {
            fn to_string(&self) -> String {
                match self {
                    $(
                        EventType::$name => stringify!($string_name),
                    )*
                }
                .to_string()
            }
        }

        impl TryFrom<String> for EventType {
            type Error = anyhow::Error;

            fn try_from(string: String) -> Result<EventType, Self::Error> {
                match string.as_str() {
                    $(
                        stringify!($string_name) => Ok(EventType::$name),
                    )*
                    other => Err(format_err!("invalid string for event type: {:?}", other))
                }
            }
        }

        impl Into<fsys::EventType> for EventType {
            fn into(self) -> fsys::EventType {
                match self {
                    $(
                        EventType::$name => fsys::EventType::$name,
                    )*
                }
            }
        }

        impl From<fsys::EventType> for EventType {
            fn from(fidl_event_type: fsys::EventType) -> Self {
                match fidl_event_type {
                    $(
                        fsys::EventType::$name => EventType::$name,
                    )*
                }
            }
        }

        impl EventType {
            /// Returns all available event types.
            pub fn values() -> Vec<EventType> {
                vec![
                    $(EventType::$name,)*
                ]
            }
        }

        impl EventPayload {
            pub fn type_(&self) -> EventType {
                match self {
                    $(
                        EventPayload::$name { .. } => EventType::$name,
                    )*
                }
            }
        }
    };
}

// Keep the event types listed below in alphabetical order!
events!([
    /// A capability exposed to the framework by a component is available.
    (CapabilityReady, capability_ready),
    /// A capability is being requested by a component and requires routing.
    /// The event propagation system is used to supply the capability being requested.
    (CapabilityRouted, capability_routed),
    /// An instance was destroyed successfully. The instance is stopped and no longer
    /// exists in the parent's realm.
    (Destroyed, destroyed),
    /// A component instance was discovered.
    (Discovered, discovered),
    /// Destruction of an instance has begun. The instance may/may not be stopped by this point.
    /// The instance still exists in the parent's realm but will soon be removed.
    /// TODO(fxb/39417): Ensure the instance is stopped before this event.
    (MarkedForDestruction, marked_for_destruction),
    /// An instance's declaration was resolved successfully for the first time.
    (Resolved, resolved),
    /// An instance is about to be started.
    (Started, started),
    /// An instance was stopped successfully.
    /// This event must occur before Destroyed.
    (Stopped, stopped),
]);

/// The component manager calls out to objects that implement the `Hook` trait on registered
/// component manager events. Hooks block the flow of a task, and can mutate, decorate and replace
/// capabilities. This permits `Hook` to serve as a point of extensibility for the component
/// manager.
#[async_trait]
pub trait Hook: Send + Sync {
    async fn on(self: Arc<Self>, event: &Event) -> Result<(), ModelError>;
}

/// An object registers a hook into a component manager event via a `HooksRegistration` object.
/// A single object may register for multiple events through a vector of `EventType`. `Hooks`
/// does not retain the callback. The hook is lazily removed when the callback object loses
/// strong references.
pub struct HooksRegistration {
    name: &'static str,
    events: Vec<EventType>,
    callback: Weak<dyn Hook>,
}

impl HooksRegistration {
    pub fn new(
        name: &'static str,
        events: Vec<EventType>,
        callback: Weak<dyn Hook>,
    ) -> HooksRegistration {
        Self { name, events, callback }
    }
}

#[derive(Clone)]
pub enum EventPayload {
    // Keep the events listed below in alphabetical order!
    CapabilityReady {
        path: String,
        node: NodeProxy,
    },
    CapabilityRouted {
        source: CapabilitySource,
        // Events are passed to hooks as immutable borrows. In order to mutate,
        // a field within an Event, interior mutability is employed here with
        // a Mutex.
        capability_provider: Arc<Mutex<Option<Box<dyn CapabilityProvider>>>>,
    },
    Destroyed,
    Discovered {
        component_url: String,
    },
    MarkedForDestruction,
    Resolved {
        decl: ComponentDecl,
    },
    Started {
        component_url: String,
        runtime: RuntimeInfo,
        component_decl: ComponentDecl,
        routing_facade: RoutingFacade,
    },
    Stopped,
}

/// Information about a component's runtime provided to `Started`.
#[derive(Clone)]
pub struct RuntimeInfo {
    pub resolved_url: String,
    pub package_dir: Option<DirectoryProxy>,
    pub outgoing_dir: Option<DirectoryProxy>,
    pub runtime_dir: Option<DirectoryProxy>,
}

impl RuntimeInfo {
    pub fn from_runtime(runtime: &Runtime) -> Self {
        Self {
            resolved_url: runtime.resolved_url.clone(),
            package_dir: runtime.namespace.as_ref().and_then(|n| clone_dir(n.package_dir.as_ref())),
            outgoing_dir: clone_dir(runtime.outgoing_dir.as_ref()),
            runtime_dir: clone_dir(runtime.runtime_dir.as_ref()),
        }
    }
}

// TODO(fsamuel): We should probably preserve the original error messages
// instead of dropping them.
fn clone_dir(dir: Option<&DirectoryProxy>) -> Option<DirectoryProxy> {
    dir.and_then(|d| io_util::clone_directory(d, fio::CLONE_FLAG_SAME_RIGHTS).ok())
}

impl fmt::Debug for EventPayload {
    fn fmt(&self, fmt: &mut fmt::Formatter) -> fmt::Result {
        let mut formatter = fmt.debug_struct("EventPayload");
        formatter.field("type", &self.type_());
        match self {
            EventPayload::CapabilityReady { path, .. } => formatter.field("path", &path).finish(),
            EventPayload::CapabilityRouted { source: capability, .. } => {
                formatter.field("capability", &capability).finish()
            }
            EventPayload::Discovered { component_url } => {
                formatter.field("component_url", component_url).finish()
            }
            EventPayload::Started { component_decl, .. } => {
                formatter.field("component_decl", &component_decl).finish()
            }
            EventPayload::Resolved { decl } => formatter.field("decl", decl).finish(),
            EventPayload::Destroyed
            | EventPayload::MarkedForDestruction
            | EventPayload::Stopped => formatter.finish(),
        }
    }
}

#[derive(Clone, Debug)]
pub struct Event {
    /// Each event has a unique 64-bit integer assigned to it
    pub id: u64,

    /// Moniker of realm that this event applies to
    pub target_moniker: AbsoluteMoniker,

    /// Payload of the event
    pub payload: EventPayload,
}

impl Event {
    pub fn new(target_moniker: AbsoluteMoniker, payload: EventPayload) -> Self {
        // Generate a random 64-bit integer to identify this event
        let id = random::<u64>();
        Self { id, target_moniker, payload }
    }
}

/// This is a collection of hooks to component manager events.
pub struct Hooks {
    parent: Option<Arc<Hooks>>,
    hooks_map: Mutex<HashMap<EventType, Vec<HookEntry>>>,
}

impl Hooks {
    pub fn new(parent: Option<Arc<Hooks>>) -> Self {
        Self { parent, hooks_map: Mutex::new(HashMap::new()) }
    }

    pub async fn install(&self, hooks: Vec<HooksRegistration>) {
        let mut hooks_map = self.hooks_map.lock().await;
        for hook in hooks {
            'event_type: for event in hook.events {
                let existing_hooks = &mut hooks_map.entry(event).or_insert(vec![]);

                for existing_hook in existing_hooks.iter() {
                    // If this hook has already been installed, skip to next event type.
                    if existing_hook.callback.ptr_eq(&hook.callback) {
                        break 'event_type;
                    }
                }

                existing_hooks.push(HookEntry { name: hook.name, callback: hook.callback.clone() });
            }
        }
    }

    pub fn dispatch<'a>(&'a self, event: &'a Event) -> BoxFuture<Result<(), ModelError>> {
        Box::pin(async move {
            // Trace event dispatch
            let event_type = format!("{:?}", event.payload.type_());
            let target_moniker = event.target_moniker.to_string();
            trace::duration!(
                "component_manager",
                "hooks:dispatch",
                "event_type" => event_type.as_str(),
                "target_moniker" => target_moniker.as_str()
            );

            let hooks = {
                trace::duration!(
                   "component_manager",
                   "hooks:upgrade_dedup",
                   "event_type" => event_type.as_ref(),
                   "target_moniker" => target_moniker.as_ref()
                );
                // We must upgrade our weak references to hooks to strong ones before we can
                // call out to them. Since hooks are deduped at install time, we do not need
                // to worry about that here.
                let mut strong_hooks = vec![];
                let mut hooks_map = self.hooks_map.lock().await;
                if let Some(hooks) = hooks_map.get_mut(&event.payload.type_()) {
                    hooks.retain(|hook| {
                        if let Some(callback) = hook.callback.upgrade() {
                            strong_hooks
                                .push(StrongHookEntry { name: hook.name.clone(), callback });
                            true
                        } else {
                            false
                        }
                    });
                }
                strong_hooks
            };
            for hook in hooks.into_iter() {
                trace::duration!(
                    "component_manager",
                    "hooks:on",
                    "event_type" => event_type.as_ref(),
                    "target_moniker" => target_moniker.as_ref(),
                    "name" => (*hook.name).as_ref()
                );
                hook.callback.on(event).await?;
            }

            if let Some(parent) = &self.parent {
                trace::duration!(
                    "component_manager",
                    "hooks:parent_dispatch",
                    "event_type" => event_type.as_ref(),
                    "target_moniker" => target_moniker.as_ref()
                );
                parent.dispatch(event).await?;
            }

            Ok(())
        })
    }
}

// Holds a Weak pointer to the Hook.
struct HookEntry {
    pub name: &'static str,
    pub callback: Weak<dyn Hook>,
}

// Holds a Strong pointer to the Hook. This is produced on dispatch when upgrading
// Weak pointers.
struct StrongHookEntry {
    pub name: &'static str,
    pub callback: Arc<dyn Hook>,
}

#[cfg(test)]
mod tests {
    use {super::*, std::sync::Arc};

    #[derive(Clone)]
    struct EventLog {
        log: Arc<Mutex<Vec<String>>>,
    }

    impl EventLog {
        pub fn new() -> Self {
            Self { log: Arc::new(Mutex::new(Vec::new())) }
        }

        pub async fn append(&self, item: String) {
            let mut log = self.log.lock().await;
            log.push(item);
        }

        pub async fn get(&self) -> Vec<String> {
            self.log.lock().await.clone()
        }
    }

    struct CallCounter {
        name: String,
        logger: Option<EventLog>,
        call_count: Mutex<i32>,
    }

    impl CallCounter {
        pub fn new(name: &str, logger: Option<EventLog>) -> Arc<Self> {
            Arc::new(Self { name: name.to_string(), logger, call_count: Mutex::new(0) })
        }

        pub async fn count(&self) -> i32 {
            *self.call_count.lock().await
        }

        async fn on_discovered_async(&self) -> Result<(), ModelError> {
            let mut call_count = self.call_count.lock().await;
            *call_count += 1;
            if let Some(logger) = &self.logger {
                let fut = logger.append(format!("{}::OnDiscovered", self.name));
                fut.await;
            }
            Ok(())
        }
    }

    #[async_trait]
    impl Hook for CallCounter {
        async fn on(self: Arc<Self>, event: &Event) -> Result<(), ModelError> {
            if let EventPayload::Discovered { .. } = event.payload {
                self.on_discovered_async().await?;
            }
            Ok(())
        }
    }

    fn log(v: Vec<&str>) -> Vec<String> {
        v.iter().map(|s| s.to_string()).collect()
    }

    // This test verifies that a hook cannot be installed twice.
    #[fuchsia_async::run_singlethreaded(test)]
    async fn install_hook_twice() {
        // CallCounter counts the number of DynamicChildAdded events it receives.
        // It should only ever receive one.
        let call_counter = CallCounter::new("CallCounter", None);
        let hooks = Hooks::new(None);

        // Attempt to install CallCounter twice.
        hooks
            .install(vec![HooksRegistration::new(
                "FirstInstall",
                vec![EventType::Discovered],
                Arc::downgrade(&call_counter) as Weak<dyn Hook>,
            )])
            .await;
        hooks
            .install(vec![HooksRegistration::new(
                "SecondInstall",
                vec![EventType::Discovered],
                Arc::downgrade(&call_counter) as Weak<dyn Hook>,
            )])
            .await;

        let root_component_url = "test:///root".to_string();
        let event = Event::new(
            AbsoluteMoniker::root(),
            EventPayload::Discovered { component_url: root_component_url },
        );
        hooks.dispatch(&event).await.expect("Unable to call hooks.");
        assert_eq!(1, call_counter.count().await);
    }

    // This test verifies that events propagate from child_hooks to parent_hooks.
    #[fuchsia_async::run_singlethreaded(test)]
    async fn event_propagation() {
        let parent_hooks = Arc::new(Hooks::new(None));
        let child_hooks = Hooks::new(Some(parent_hooks.clone()));

        let event_log = EventLog::new();
        let parent_call_counter = CallCounter::new("ParentCallCounter", Some(event_log.clone()));
        parent_hooks
            .install(vec![HooksRegistration::new(
                "ParentHook",
                vec![EventType::Discovered],
                Arc::downgrade(&parent_call_counter) as Weak<dyn Hook>,
            )])
            .await;

        let child_call_counter = CallCounter::new("ChildCallCounter", Some(event_log.clone()));
        child_hooks
            .install(vec![HooksRegistration::new(
                "ChildHook",
                vec![EventType::Discovered],
                Arc::downgrade(&child_call_counter) as Weak<dyn Hook>,
            )])
            .await;

        assert_eq!(1, Arc::strong_count(&parent_call_counter));
        assert_eq!(1, Arc::strong_count(&child_call_counter));

        let root_component_url = "test:///root".to_string();
        let event = Event::new(
            AbsoluteMoniker::root(),
            EventPayload::Discovered { component_url: root_component_url },
        );
        child_hooks.dispatch(&event).await.expect("Unable to call hooks.");
        // parent_call_counter gets informed of the event on child_hooks even though it has
        // been installed on parent_hooks.
        assert_eq!(1, parent_call_counter.count().await);
        // child_call_counter should be called only once.
        assert_eq!(1, child_call_counter.count().await);

        // Dropping the child_call_counter should drop the weak pointer to it in hooks
        // as well.
        drop(child_call_counter);

        // Dispatching an event on child_hooks will not call out to child_call_counter
        // because it has been destroyed by the call to drop above.
        child_hooks.dispatch(&event).await.expect("Unable to call hooks.");

        // ChildCallCounter should be called before ParentCallCounter.
        assert_eq!(
            log(vec![
                "ChildCallCounter::OnDiscovered",
                "ParentCallCounter::OnDiscovered",
                "ParentCallCounter::OnDiscovered",
            ]),
            event_log.get().await
        );
    }
}
