// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::capability::NamespaceCapabilities,
    crate::config::RuntimeConfig,
    crate::model::{
        actions::Action,
        binding::Binder,
        context::ModelContext,
        environment::Environment,
        error::ModelError,
        realm::{BindReason, ComponentManagerRealm, Realm},
    },
    moniker::AbsoluteMoniker,
    std::sync::Arc,
};

/// Parameters for initializing a component model, particularly the root of the component
/// instance tree.
pub struct ModelParams {
    // TODO(viktard): Merge into RuntimeConfig
    /// The URL of the root component.
    pub root_component_url: String,
    /// The environment provided to the root realm.
    pub root_environment: Environment,
    /// Global runtime configuration for the component_manager.
    pub runtime_config: Arc<RuntimeConfig>,
    /// The namespace capabilities offered by component manager
    pub namespace_capabilities: NamespaceCapabilities,
}

/// The component model holds authoritative state about a tree of component instances, including
/// each instance's identity, lifecycle, capabilities, and topological relationships.  It also
/// provides operations for instantiating, destroying, querying, and controlling component
/// instances at runtime.
pub struct Model {
    pub root_realm: Arc<Realm>,
    _context: Arc<ModelContext>,
    _component_manager_realm: Arc<ComponentManagerRealm>,
}

impl Model {
    /// Creates a new component model and initializes its topology.
    pub fn new(params: ModelParams) -> Model {
        let component_manager_realm =
            Arc::new(ComponentManagerRealm::new(params.namespace_capabilities));
        let context = Arc::new(ModelContext::new(params.runtime_config));
        let root_realm = Arc::new(Realm::new_root_realm(
            params.root_environment,
            Arc::downgrade(&context),
            Arc::downgrade(&component_manager_realm),
            params.root_component_url,
        ));
        Model { root_realm, _context: context, _component_manager_realm: component_manager_realm }
    }

    /// Looks up a realm by absolute moniker. The component instance in the realm will be resolved
    /// if that has not already happened.
    pub async fn look_up_realm(
        &self,
        look_up_abs_moniker: &AbsoluteMoniker,
    ) -> Result<Arc<Realm>, ModelError> {
        let mut cur_realm = self.root_realm.clone();
        for moniker in look_up_abs_moniker.path().iter() {
            cur_realm = {
                let cur_state = cur_realm.lock_resolved_state().await?;
                if let Some(r) = cur_state.all_child_realms().get(moniker) {
                    r.clone()
                } else {
                    return Err(ModelError::instance_not_found(look_up_abs_moniker.clone()));
                }
            };
        }
        let _ = cur_realm.lock_resolved_state().await?;
        Ok(cur_realm)
    }

    /// Binds to the root realm, starting the component tree
    pub async fn start(self: &Arc<Model>) {
        let root_moniker = AbsoluteMoniker::root();
        if let Err(e) = self.bind(&root_moniker, &BindReason::Root).await {
            // If we fail binding to the root realm, but the root realm is being shutdown, that's
            // ok. The system is tearing down, so it doesn't matter any more if we never got
            // everything started that we wanted to.
            let action_set = self.root_realm.lock_actions().await;
            if !action_set.contains(&Action::Shutdown) {
                panic!(
                    "failed to bind to root component {}: {:?}",
                    self.root_realm.component_url, e
                );
            }
        }
    }
}

#[cfg(test)]
pub mod tests {
    use {
        crate::{
            config::RuntimeConfig,
            model::actions::Action,
            model::testing::test_helpers::{new_test_model, ComponentDeclBuilder, TestModelResult},
        },
        fidl_fuchsia_sys2 as fsys, fuchsia_async as fasync,
    };

    #[fasync::run_singlethreaded(test)]
    async fn shutting_down_when_start_fails() {
        let components = vec![(
            "root",
            ComponentDeclBuilder::new()
                .add_child(cm_rust::ChildDecl {
                    name: "bad-scheme".to_string(),
                    url: "bad-scheme://sdf".to_string(),
                    startup: fsys::StartupMode::Eager,
                    environment: None,
                })
                .build(),
        )];

        let TestModelResult { model, .. } =
            new_test_model("root", components, RuntimeConfig::default()).await;

        let _ = model
            .root_realm
            .lock_actions()
            .await
            .register_inner(&model.root_realm, Action::Shutdown);

        model.start().await;
    }

    #[should_panic]
    #[fasync::run_singlethreaded(test)]
    async fn not_shutting_down_when_start_fails() {
        let components = vec![(
            "root",
            ComponentDeclBuilder::new()
                .add_child(cm_rust::ChildDecl {
                    name: "bad-scheme".to_string(),
                    url: "bad-scheme://sdf".to_string(),
                    startup: fsys::StartupMode::Eager,
                    environment: None,
                })
                .build(),
        )];

        let TestModelResult { model, .. } =
            new_test_model("root", components, RuntimeConfig::default()).await;

        model.start().await;
    }
}
