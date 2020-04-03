// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.cti

use {
    crate::{
        capability::{CapabilityProvider, CapabilitySource, FrameworkCapability},
        model::{
            error::ModelError,
            hooks::{Event, EventPayload, EventType, Hook, HooksRegistration},
            model::{ComponentManagerConfig, Model},
            moniker::{AbsoluteMoniker, PartialMoniker},
            realm::Realm,
        },
    },
    anyhow::Error,
    async_trait::async_trait,
    cm_fidl_validator,
    cm_rust::{CapabilityPath, FidlIntoNative},
    fidl::endpoints::ServerEnd,
    fidl_fuchsia_component as fcomponent,
    fidl_fuchsia_io::DirectoryMarker,
    fidl_fuchsia_sys2 as fsys, fuchsia_async as fasync, fuchsia_zircon as zx,
    futures::prelude::*,
    lazy_static::lazy_static,
    log::*,
    std::{
        cmp,
        convert::TryInto,
        path::PathBuf,
        sync::{Arc, Weak},
    },
};

lazy_static! {
    pub static ref REALM_SERVICE: CapabilityPath = "/svc/fuchsia.sys2.Realm".try_into().unwrap();
}

// The default implementation for framework services.
pub struct RealmCapabilityProvider {
    scope_moniker: AbsoluteMoniker,
    host: Arc<RealmCapabilityHost>,
}

impl RealmCapabilityProvider {
    pub fn new(scope_moniker: AbsoluteMoniker, host: Arc<RealmCapabilityHost>) -> Self {
        Self { scope_moniker, host }
    }
}

#[async_trait]
impl CapabilityProvider for RealmCapabilityProvider {
    async fn open(
        self: Box<Self>,
        _flags: u32,
        _open_mode: u32,
        _relative_path: PathBuf,
        server_end: zx::Channel,
    ) -> Result<(), ModelError> {
        let stream = ServerEnd::<fsys::RealmMarker>::new(server_end)
            .into_stream()
            .expect("could not convert channel into stream");
        let scope_moniker = self.scope_moniker.clone();
        let host = self.host.clone();
        fasync::spawn(async move {
            if let Err(e) = host.serve(scope_moniker, stream).await {
                // TODO: Set an epitaph to indicate this was an unexpected error.
                warn!("serve_realm failed: {:?}", e);
            }
        });
        Ok(())
    }
}

#[derive(Clone)]
pub struct RealmCapabilityHost {
    model: Arc<Model>,
    config: ComponentManagerConfig,
}

// `RealmCapabilityHost` is a `Hook` that serves the `Realm` FIDL protocol.
impl RealmCapabilityHost {
    pub fn new(model: Arc<Model>, config: ComponentManagerConfig) -> Self {
        Self { model, config }
    }

    pub fn hooks(self: &Arc<Self>) -> Vec<HooksRegistration> {
        vec![HooksRegistration::new(
            "RealmCapabilityHost",
            vec![EventType::CapabilityRouted],
            Arc::downgrade(self) as Weak<dyn Hook>,
        )]
    }

    pub async fn serve(
        &self,
        scope_moniker: AbsoluteMoniker,
        mut stream: fsys::RealmRequestStream,
    ) -> Result<(), Error> {
        // We only need to look up the realm matching this scope.
        // These realm operations should all work, even if the scope realm is not running.
        // A successful call to BindChild will cause the scope realm to start running.
        let realm = Arc::downgrade(&self.model.look_up_realm(&scope_moniker).await?);
        while let Some(request) = stream.try_next().await? {
            let realm = match realm.upgrade() {
                Some(r) => r,
                None => {
                    break;
                }
            };
            match request {
                fsys::RealmRequest::CreateChild { responder, collection, decl } => {
                    let mut res = Self::create_child(realm, collection, decl).await;
                    responder.send(&mut res)?;
                }
                fsys::RealmRequest::BindChild { responder, child, exposed_dir } => {
                    let mut res = Self::bind_child(realm, child, exposed_dir).await;
                    responder.send(&mut res)?;
                }
                fsys::RealmRequest::DestroyChild { responder, child } => {
                    let mut res = Self::destroy_child(realm, child).await;
                    responder.send(&mut res)?;
                }
                fsys::RealmRequest::ListChildren { responder, collection, iter } => {
                    let mut res = Self::list_children(&self.config, realm, collection, iter).await;
                    responder.send(&mut res)?;
                }
            }
        }
        Ok(())
    }

    async fn create_child(
        realm: Arc<Realm>,
        collection: fsys::CollectionRef,
        child_decl: fsys::ChildDecl,
    ) -> Result<(), fcomponent::Error> {
        cm_fidl_validator::validate_child(&child_decl)
            .map_err(|_| fcomponent::Error::InvalidArguments)?;
        let child_decl = child_decl.fidl_into_native();
        realm.add_dynamic_child(collection.name, &child_decl).await.map_err(|e| match e {
            ModelError::InstanceAlreadyExists { .. } => fcomponent::Error::InstanceAlreadyExists,
            ModelError::CollectionNotFound { .. } => fcomponent::Error::CollectionNotFound,
            ModelError::Unsupported { .. } => fcomponent::Error::Unsupported,
            e => {
                error!("add_dynamic_child() failed: {:?}", e);
                fcomponent::Error::Internal
            }
        })?;
        Ok(())
    }

    async fn bind_child(
        realm: Arc<Realm>,
        child: fsys::ChildRef,
        exposed_dir: ServerEnd<DirectoryMarker>,
    ) -> Result<(), fcomponent::Error> {
        let partial_moniker = PartialMoniker::new(child.name, child.collection);
        let child_realm = {
            let realm_state = realm.lock_resolved_state().await.map_err(|e| match e {
                ModelError::ResolverError { err } => {
                    debug!("failed to resolve: {:?}", err);
                    fcomponent::Error::InstanceCannotResolve
                }
                e => {
                    error!("failed to resolve RealmState: {}", e);
                    fcomponent::Error::Internal
                }
            })?;
            realm_state.get_live_child_realm(&partial_moniker).map(|r| r.clone())
        };
        if let Some(child_realm) = child_realm {
            child_realm
                .bind()
                .await
                .map_err(|e| match e {
                    ModelError::ResolverError { err } => {
                        debug!("failed to resolve child: {:?}", err);
                        fcomponent::Error::InstanceCannotResolve
                    }
                    ModelError::RunnerError { err } => {
                        debug!("failed to start child: {:?}", err);
                        fcomponent::Error::InstanceCannotStart
                    }
                    e => {
                        error!("bind() failed: {:?}", e);
                        fcomponent::Error::Internal
                    }
                })?
                .open_exposed(exposed_dir.into_channel())
                .await
                .map_err(|e| {
                    error!("open_exposed() failed: {:?}", e);
                    fcomponent::Error::Internal
                })?;
        } else {
            return Err(fcomponent::Error::InstanceNotFound);
        }
        Ok(())
    }

    async fn destroy_child(
        realm: Arc<Realm>,
        child: fsys::ChildRef,
    ) -> Result<(), fcomponent::Error> {
        child.collection.as_ref().ok_or(fcomponent::Error::InvalidArguments)?;
        let partial_moniker = PartialMoniker::new(child.name, child.collection);
        let _ = realm.remove_dynamic_child(&partial_moniker).await.map_err(|e| match e {
            ModelError::InstanceNotFoundInRealm { .. } => fcomponent::Error::InstanceNotFound,
            ModelError::Unsupported { .. } => fcomponent::Error::Unsupported,
            e => {
                error!("remove_dynamic_child() failed: {:?}", e);
                fcomponent::Error::Internal
            }
        })?;
        Ok(())
    }

    async fn list_children(
        config: &ComponentManagerConfig,
        realm: Arc<Realm>,
        collection: fsys::CollectionRef,
        iter: ServerEnd<fsys::ChildIteratorMarker>,
    ) -> Result<(), fcomponent::Error> {
        let state = realm.lock_resolved_state().await.map_err(|e| {
            error!("failed to resolve RealmState: {:?}", e);
            fcomponent::Error::Internal
        })?;
        let decl = state.decl();
        let _ = decl
            .find_collection(&collection.name)
            .ok_or_else(|| fcomponent::Error::CollectionNotFound)?;
        let mut children: Vec<_> = state
            .live_child_realms()
            .filter_map(|(m, _)| match m.collection() {
                Some(c) => {
                    if c == collection.name {
                        Some(fsys::ChildRef {
                            name: m.name().to_string(),
                            collection: m.collection().map(|s| s.to_string()),
                        })
                    } else {
                        None
                    }
                }
                _ => None,
            })
            .collect();
        children.sort_unstable_by(|a, b| {
            let a = &a.name;
            let b = &b.name;
            if a == b {
                cmp::Ordering::Equal
            } else if a < b {
                cmp::Ordering::Less
            } else {
                cmp::Ordering::Greater
            }
        });
        let stream = iter.into_stream().expect("could not convert iterator channel into stream");
        let batch_size = config.list_children_batch_size;
        fasync::spawn(async move {
            if let Err(e) = Self::serve_child_iterator(children, stream, batch_size).await {
                // TODO: Set an epitaph to indicate this was an unexpected error.
                warn!("serve_child_iterator failed: {:?}", e);
            }
        });
        Ok(())
    }

    async fn serve_child_iterator(
        mut children: Vec<fsys::ChildRef>,
        mut stream: fsys::ChildIteratorRequestStream,
        batch_size: usize,
    ) -> Result<(), Error> {
        while let Some(request) = stream.try_next().await? {
            match request {
                fsys::ChildIteratorRequest::Next { responder } => {
                    let n_to_send = std::cmp::min(children.len(), batch_size);
                    let mut res: Vec<_> = children.drain(..n_to_send).collect();
                    responder.send(&mut res.iter_mut())?;
                }
            }
        }
        Ok(())
    }

    async fn on_scoped_framework_capability_routed_async<'a>(
        self: Arc<Self>,
        scope_moniker: AbsoluteMoniker,
        capability: &'a FrameworkCapability,
        capability_provider: Option<Box<dyn CapabilityProvider>>,
    ) -> Result<Option<Box<dyn CapabilityProvider>>, ModelError> {
        // If some other capability has already been installed, then there's nothing to
        // do here.
        match (&capability_provider, capability) {
            (None, FrameworkCapability::Protocol(capability_path))
                if *capability_path == *REALM_SERVICE =>
            {
                return Ok(Some(
                    Box::new(RealmCapabilityProvider::new(scope_moniker, self.clone()))
                        as Box<dyn CapabilityProvider>,
                ));
            }
            _ => return Ok(capability_provider),
        }
    }
}

#[async_trait]
impl Hook for RealmCapabilityHost {
    async fn on(self: Arc<Self>, event: &Event) -> Result<(), ModelError> {
        if let EventPayload::CapabilityRouted {
            source: CapabilitySource::Framework { capability, scope_moniker: Some(scope_moniker) },
            capability_provider,
        } = &event.payload
        {
            let mut capability_provider = capability_provider.lock().await;
            *capability_provider = self
                .on_scoped_framework_capability_routed_async(
                    scope_moniker.clone(),
                    &capability,
                    capability_provider.take(),
                )
                .await?;
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use {
        crate::{
            builtin_environment::BuiltinEnvironment,
            model::{
                binding::Binder,
                events::{source::EventSource, stream::EventStream},
                model::ModelParams,
                moniker::AbsoluteMoniker,
                resolver::ResolverRegistry,
                testing::{mocks::*, out_dir::OutDir, test_helpers, test_helpers::*, test_hook::*},
            },
            startup,
        },
        cm_rust::{
            self, CapabilityName, CapabilityPath, ChildDecl, ExposeDecl, ExposeProtocolDecl,
            ExposeSource, ExposeTarget, NativeIntoFidl,
        },
        fidl::endpoints,
        fidl_fidl_examples_echo as echo,
        fidl_fuchsia_io::MODE_TYPE_SERVICE,
        fuchsia_async as fasync,
        io_util::OPEN_RIGHT_READABLE,
        std::collections::HashSet,
        std::convert::TryFrom,
        std::path::PathBuf,
    };

    struct RealmCapabilityTest {
        pub model: Arc<Model>,
        pub builtin_environment: Arc<BuiltinEnvironment>,
        realm: Arc<Realm>,
        realm_proxy: fsys::RealmProxy,
        hook: Arc<TestHook>,
        events_data: Option<EventsData>,
    }

    struct EventsData {
        _event_source: EventSource,
        event_stream: EventStream,
    }

    impl RealmCapabilityTest {
        async fn new(
            mock_resolver: MockResolver,
            mock_runner: Arc<MockRunner>,
            realm_moniker: AbsoluteMoniker,
            events: Vec<CapabilityName>,
        ) -> Self {
            // Init model.
            let mut resolver = ResolverRegistry::new();
            resolver.register("test".to_string(), Box::new(mock_resolver));
            let mut config = ComponentManagerConfig::default();
            config.list_children_batch_size = 2;
            let startup_args = startup::Arguments {
                use_builtin_process_launcher: false,
                root_component_url: "".to_string(),
                debug: false,
            };
            let model = Arc::new(Model::new(ModelParams {
                root_component_url: "test:///root".to_string(),
                root_resolver_registry: resolver,
            }));
            let builtin_environment = Arc::new(
                BuiltinEnvironment::new(
                    &startup_args,
                    &model,
                    config,
                    &vec![(test_helpers::TEST_RUNNER_NAME.into(), mock_runner as _)]
                        .into_iter()
                        .collect(),
                )
                .await
                .expect("failed to set up builtin environment"),
            );
            let builtin_environment_inner = builtin_environment.clone();

            let hook = Arc::new(TestHook::new());
            let hooks = hook.hooks();

            let events_data = if events.is_empty() {
                None
            } else {
                let mut event_source = builtin_environment_inner
                    .event_source_factory
                    .create_for_debug()
                    .await
                    .expect("created event source");
                let event_stream =
                    event_source.subscribe(events).await.expect("subscribe to event stream");
                event_source.start_component_tree().await;
                Some(EventsData { _event_source: event_source, event_stream })
            };

            model.root_realm.hooks.install(hooks).await;

            // Look up and bind to realm.
            let realm = model.bind(&realm_moniker).await.expect("failed to bind to realm");

            // Host framework service.
            let (realm_proxy, stream) =
                endpoints::create_proxy_and_stream::<fsys::RealmMarker>().unwrap();
            {
                fasync::spawn(async move {
                    builtin_environment_inner
                        .realm_capability_host
                        .serve(realm_moniker, stream)
                        .await
                        .expect("failed serving realm service");
                });
            }
            RealmCapabilityTest {
                model,
                builtin_environment,
                realm,
                realm_proxy,
                hook,
                events_data,
            }
        }

        fn event_stream(&mut self) -> Option<&mut EventStream> {
            self.events_data.as_mut().map(|data| &mut data.event_stream)
        }
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn create_dynamic_child() {
        // Set up model and realm service.
        let mut mock_resolver = MockResolver::new();
        let mock_runner = Arc::new(MockRunner::new());
        mock_resolver.add_component(
            "root",
            ComponentDeclBuilder::new()
                .add_lazy_child("system")
                .offer_runner_to_children(TEST_RUNNER_NAME)
                .build(),
        );
        mock_resolver.add_component(
            "system",
            ComponentDeclBuilder::new()
                .add_collection("coll", fsys::Durability::Transient)
                .offer_runner_to_children(TEST_RUNNER_NAME)
                .build(),
        );
        let test = RealmCapabilityTest::new(
            mock_resolver,
            mock_runner.clone(),
            vec!["system:0"].into(),
            vec![],
        )
        .await;

        // Create children "a" and "b" in collection.
        let mut collection_ref = fsys::CollectionRef { name: "coll".to_string() };
        let res = test.realm_proxy.create_child(&mut collection_ref, child_decl("a")).await;
        let _ = res.expect("failed to create child a").expect("failed to create child a");

        let mut collection_ref = fsys::CollectionRef { name: "coll".to_string() };
        let res = test.realm_proxy.create_child(&mut collection_ref, child_decl("b")).await;
        let _ = res.expect("failed to create child b").expect("failed to create child b");

        // Verify that the component topology matches expectations.
        let actual_children = get_live_children(&test.realm).await;
        let mut expected_children: HashSet<PartialMoniker> = HashSet::new();
        expected_children.insert("coll:a".into());
        expected_children.insert("coll:b".into());
        assert_eq!(actual_children, expected_children);
        assert_eq!("(system(coll:a,coll:b))", test.hook.print());
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn create_dynamic_child_errors() {
        let mut mock_resolver = MockResolver::new();
        mock_resolver.add_component(
            "root",
            ComponentDeclBuilder::new()
                .add_lazy_child("system")
                .offer_runner_to_children(TEST_RUNNER_NAME)
                .build(),
        );
        mock_resolver.add_component(
            "system",
            ComponentDeclBuilder::new()
                .add_collection("coll", fsys::Durability::Transient)
                .add_collection("pcoll", fsys::Durability::Persistent)
                .offer_runner_to_children(TEST_RUNNER_NAME)
                .build(),
        );
        let test = RealmCapabilityTest::new(
            mock_resolver,
            Arc::new(MockRunner::new()),
            vec!["system:0"].into(),
            vec![],
        )
        .await;

        // Invalid arguments.
        {
            let mut collection_ref = fsys::CollectionRef { name: "coll".to_string() };
            let child_decl = fsys::ChildDecl {
                name: Some("a".to_string()),
                url: None,
                startup: Some(fsys::StartupMode::Lazy),
                environment: None,
            };
            let err = test
                .realm_proxy
                .create_child(&mut collection_ref, child_decl)
                .await
                .expect("fidl call failed")
                .expect_err("unexpected success");
            assert_eq!(err, fcomponent::Error::InvalidArguments);
        }

        // Instance already exists.
        {
            let mut collection_ref = fsys::CollectionRef { name: "coll".to_string() };
            let res = test.realm_proxy.create_child(&mut collection_ref, child_decl("a")).await;
            let _ = res.expect("failed to create child a");
            let mut collection_ref = fsys::CollectionRef { name: "coll".to_string() };
            let err = test
                .realm_proxy
                .create_child(&mut collection_ref, child_decl("a"))
                .await
                .expect("fidl call failed")
                .expect_err("unexpected success");
            assert_eq!(err, fcomponent::Error::InstanceAlreadyExists);
        }

        // Collection not found.
        {
            let mut collection_ref = fsys::CollectionRef { name: "nonexistent".to_string() };
            let err = test
                .realm_proxy
                .create_child(&mut collection_ref, child_decl("a"))
                .await
                .expect("fidl call failed")
                .expect_err("unexpected success");
            assert_eq!(err, fcomponent::Error::CollectionNotFound);
        }

        // Unsupported.
        {
            let mut collection_ref = fsys::CollectionRef { name: "pcoll".to_string() };
            let err = test
                .realm_proxy
                .create_child(&mut collection_ref, child_decl("a"))
                .await
                .expect("fidl call failed")
                .expect_err("unexpected success");
            assert_eq!(err, fcomponent::Error::Unsupported);
        }
        {
            let mut collection_ref = fsys::CollectionRef { name: "coll".to_string() };
            let child_decl = fsys::ChildDecl {
                name: Some("b".to_string()),
                url: Some("test:///b".to_string()),
                startup: Some(fsys::StartupMode::Eager),
                environment: None,
            };
            let err = test
                .realm_proxy
                .create_child(&mut collection_ref, child_decl)
                .await
                .expect("fidl call failed")
                .expect_err("unexpected success");
            assert_eq!(err, fcomponent::Error::Unsupported);
        }
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn destroy_dynamic_child() {
        // Set up model and realm service.
        let mut mock_resolver = MockResolver::new();
        mock_resolver.add_component(
            "root",
            ComponentDeclBuilder::new()
                .add_lazy_child("system")
                .offer_runner_to_children(TEST_RUNNER_NAME)
                .build(),
        );
        mock_resolver.add_component(
            "system",
            ComponentDeclBuilder::new()
                .add_collection("coll", fsys::Durability::Transient)
                .offer_runner_to_children(TEST_RUNNER_NAME)
                .build(),
        );
        mock_resolver.add_component("a", component_decl_with_test_runner());
        mock_resolver.add_component("b", component_decl_with_test_runner());

        let events = vec![EventType::MarkedForDestruction.into(), EventType::Destroyed.into()];
        let mut test = RealmCapabilityTest::new(
            mock_resolver,
            Arc::new(MockRunner::new()),
            vec!["system:0"].into(),
            events,
        )
        .await;

        // Create children "a" and "b" in collection, and bind to them.
        for name in &["a", "b"] {
            let mut collection_ref = fsys::CollectionRef { name: "coll".to_string() };
            let res = test.realm_proxy.create_child(&mut collection_ref, child_decl(name)).await;
            let _ = res
                .unwrap_or_else(|_| panic!("failed to create child {}", name))
                .unwrap_or_else(|_| panic!("failed to create child {}", name));
            let mut child_ref =
                fsys::ChildRef { name: name.to_string(), collection: Some("coll".to_string()) };
            let (_dir_proxy, server_end) = endpoints::create_proxy::<DirectoryMarker>().unwrap();
            let res = test.realm_proxy.bind_child(&mut child_ref, server_end).await;
            let _ = res
                .unwrap_or_else(|_| panic!("failed to bind to child {}", name))
                .unwrap_or_else(|_| panic!("failed to bind to child {}", name));
        }

        let child_realm = get_live_child(&test.realm, "coll:a").await;
        let instance_id = get_instance_id(&test.realm, "coll:a").await;
        assert_eq!("(system(coll:a,coll:b))", test.hook.print());
        assert_eq!(child_realm.component_url, "test:///a".to_string());
        assert_eq!(instance_id, 1);

        // Destroy "a". "a" is no longer live from the client's perspective, although it's still
        // being destroyed.
        let mut child_ref =
            fsys::ChildRef { name: "a".to_string(), collection: Some("coll".to_string()) };
        let (f, destroy_handle) = test.realm_proxy.destroy_child(&mut child_ref).remote_handle();
        fasync::spawn(f);

        let event = test
            .event_stream()
            .unwrap()
            .wait_until(EventType::MarkedForDestruction, vec!["system:0", "coll:a:1"].into())
            .await
            .unwrap();

        // Child is not marked deleted yet.
        {
            let actual_children = get_live_children(&test.realm).await;
            let mut expected_children: HashSet<PartialMoniker> = HashSet::new();
            expected_children.insert("coll:a".into());
            expected_children.insert("coll:b".into());
            assert_eq!(actual_children, expected_children);
        }

        // The destruction of "a" was arrested during `PreDestroy`. The old "a" should still exist,
        // although it's not live.
        assert!(has_child(&test.realm, "coll:a:1").await);

        // Move past the 'PreDestroy' event for "a", and wait for destroy_child to return.
        event.resume();
        let res = destroy_handle.await;
        let _ = res.expect("failed to destroy child a").expect("failed to destroy child a");

        // Child is marked deleted now.
        {
            let actual_children = get_live_children(&test.realm).await;
            let mut expected_children: HashSet<PartialMoniker> = HashSet::new();
            expected_children.insert("coll:b".into());
            assert_eq!(actual_children, expected_children);
            assert_eq!("(system(coll:b))", test.hook.print());
        }

        // Wait until 'PostDestroy' event for "a"
        let event = test
            .event_stream()
            .unwrap()
            .wait_until(EventType::Destroyed, vec!["system:0", "coll:a:1"].into())
            .await
            .unwrap();
        event.resume();

        assert!(!has_child(&test.realm, "coll:a:1").await);

        // Recreate "a" and verify "a" is back (but it's a different "a"). The old "a" is gone
        // from the client's point of view, but it hasn't been cleaned up yet.
        let mut collection_ref = fsys::CollectionRef { name: "coll".to_string() };
        let child_decl = fsys::ChildDecl {
            name: Some("a".to_string()),
            url: Some("test:///a_alt".to_string()),
            startup: Some(fsys::StartupMode::Lazy),
            environment: None,
        };
        let res = test.realm_proxy.create_child(&mut collection_ref, child_decl).await;
        let _ = res.expect("failed to recreate child a").expect("failed to recreate child a");

        assert_eq!("(system(coll:a,coll:b))", test.hook.print());
        let child_realm = get_live_child(&test.realm, "coll:a").await;
        let instance_id = get_instance_id(&test.realm, "coll:a").await;
        assert_eq!(child_realm.component_url, "test:///a_alt".to_string());
        assert_eq!(instance_id, 3);
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn destroy_dynamic_child_errors() {
        let mut mock_resolver = MockResolver::new();
        mock_resolver.add_component(
            "root",
            ComponentDeclBuilder::new()
                .add_lazy_child("system")
                .offer_runner_to_children(TEST_RUNNER_NAME)
                .build(),
        );
        mock_resolver.add_component(
            "system",
            ComponentDeclBuilder::new()
                .add_collection("coll", fsys::Durability::Transient)
                .offer_runner_to_children(TEST_RUNNER_NAME)
                .build(),
        );
        let test = RealmCapabilityTest::new(
            mock_resolver,
            Arc::new(MockRunner::new()),
            vec!["system:0"].into(),
            vec![],
        )
        .await;

        // Create child "a" in collection.
        let mut collection_ref = fsys::CollectionRef { name: "coll".to_string() };
        let res = test.realm_proxy.create_child(&mut collection_ref, child_decl("a")).await;
        let _ = res.expect("failed to create child a").expect("failed to create child a");

        // Invalid arguments.
        {
            let mut child_ref = fsys::ChildRef { name: "a".to_string(), collection: None };
            let err = test
                .realm_proxy
                .destroy_child(&mut child_ref)
                .await
                .expect("fidl call failed")
                .expect_err("unexpected success");
            assert_eq!(err, fcomponent::Error::InvalidArguments);
        }

        // Instance not found.
        {
            let mut child_ref =
                fsys::ChildRef { name: "b".to_string(), collection: Some("coll".to_string()) };
            let err = test
                .realm_proxy
                .destroy_child(&mut child_ref)
                .await
                .expect("fidl call failed")
                .expect_err("unexpected success");
            assert_eq!(err, fcomponent::Error::InstanceNotFound);
        }
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn bind_static_child() {
        // Create a hierarchy of three components, the last with eager startup. The middle
        // component hosts and exposes the "/svc/hippo" service.
        let mut mock_resolver = MockResolver::new();
        mock_resolver.add_component(
            "root",
            ComponentDeclBuilder::new()
                .add_lazy_child("system")
                .offer_runner_to_children(TEST_RUNNER_NAME)
                .build(),
        );
        mock_resolver.add_component(
            "system",
            ComponentDeclBuilder::new()
                .expose(ExposeDecl::Protocol(ExposeProtocolDecl {
                    source: ExposeSource::Self_,
                    source_path: CapabilityPath::try_from("/svc/foo").unwrap(),
                    target_path: CapabilityPath::try_from("/svc/hippo").unwrap(),
                    target: ExposeTarget::Realm,
                }))
                .add_eager_child("eager")
                .offer_runner_to_children(TEST_RUNNER_NAME)
                .build(),
        );
        mock_resolver.add_component("eager", component_decl_with_test_runner());
        let mock_runner = Arc::new(MockRunner::new());
        let mut out_dir = OutDir::new();
        out_dir.add_echo_service(CapabilityPath::try_from("/svc/foo").unwrap());
        mock_runner.add_host_fn("test:///system_resolved", out_dir.host_fn());
        let test =
            RealmCapabilityTest::new(mock_resolver, mock_runner.clone(), vec![].into(), vec![])
                .await;

        // Bind to child and use exposed service.
        let mut child_ref = fsys::ChildRef { name: "system".to_string(), collection: None };
        let (dir_proxy, server_end) = endpoints::create_proxy::<DirectoryMarker>().unwrap();
        let res = test.realm_proxy.bind_child(&mut child_ref, server_end).await;
        let _ = res.expect("failed to bind to system").expect("failed to bind to system");
        let node_proxy = io_util::open_node(
            &dir_proxy,
            &PathBuf::from("svc/hippo"),
            OPEN_RIGHT_READABLE,
            MODE_TYPE_SERVICE,
        )
        .expect("failed to open echo service");
        let echo_proxy = echo::EchoProxy::new(node_proxy.into_channel().unwrap());
        let res = echo_proxy.echo_string(Some("hippos")).await;
        assert_eq!(res.expect("failed to use echo service"), Some("hippos".to_string()));

        // Verify that the bindings happened (including the eager binding) and the component
        // topology matches expectations.
        let expected_urls = vec![
            "test:///root_resolved".to_string(),
            "test:///system_resolved".to_string(),
            "test:///eager_resolved".to_string(),
        ];
        assert_eq!(mock_runner.urls_run(), expected_urls);
        assert_eq!("(system(eager))", test.hook.print());
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn bind_dynamic_child() {
        // Create a root component with a collection and define a component that exposes a service.
        let mut mock_resolver = MockResolver::new();
        mock_resolver.add_component(
            "root",
            ComponentDeclBuilder::new()
                .add_collection("coll", fsys::Durability::Transient)
                .offer_runner_to_children(TEST_RUNNER_NAME)
                .build(),
        );
        mock_resolver.add_component(
            "system",
            ComponentDeclBuilder::new()
                .expose(ExposeDecl::Protocol(ExposeProtocolDecl {
                    source: ExposeSource::Self_,
                    source_path: CapabilityPath::try_from("/svc/foo").unwrap(),
                    target_path: CapabilityPath::try_from("/svc/hippo").unwrap(),
                    target: ExposeTarget::Realm,
                }))
                .build(),
        );
        let mock_runner = Arc::new(MockRunner::new());
        let mut out_dir = OutDir::new();
        out_dir.add_echo_service(CapabilityPath::try_from("/svc/foo").unwrap());
        mock_runner.add_host_fn("test:///system_resolved", out_dir.host_fn());
        let test =
            RealmCapabilityTest::new(mock_resolver, mock_runner.clone(), vec![].into(), vec![])
                .await;

        // Add "system" to collection.
        let mut collection_ref = fsys::CollectionRef { name: "coll".to_string() };
        let res = test.realm_proxy.create_child(&mut collection_ref, child_decl("system")).await;
        let _ = res.expect("failed to create child system").expect("failed to create child system");

        // Bind to child and use exposed service.
        let mut child_ref =
            fsys::ChildRef { name: "system".to_string(), collection: Some("coll".to_string()) };
        let (dir_proxy, server_end) = endpoints::create_proxy::<DirectoryMarker>().unwrap();
        let res = test.realm_proxy.bind_child(&mut child_ref, server_end).await;
        let _ = res.expect("failed to bind to system").expect("failed to bind to system");
        let node_proxy = io_util::open_node(
            &dir_proxy,
            &PathBuf::from("svc/hippo"),
            OPEN_RIGHT_READABLE,
            MODE_TYPE_SERVICE,
        )
        .expect("failed to open echo service");
        let echo_proxy = echo::EchoProxy::new(node_proxy.into_channel().unwrap());
        let res = echo_proxy.echo_string(Some("hippos")).await;
        assert_eq!(res.expect("failed to use echo service"), Some("hippos".to_string()));

        // Verify that the binding happened and the component topology matches expectations.
        let expected_urls =
            vec!["test:///root_resolved".to_string(), "test:///system_resolved".to_string()];
        assert_eq!(mock_runner.urls_run(), expected_urls);
        assert_eq!("(coll:system)", test.hook.print());
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn bind_child_errors() {
        let mut mock_resolver = MockResolver::new();
        mock_resolver.add_component(
            "root",
            ComponentDeclBuilder::new()
                .add_lazy_child("system")
                .add_lazy_child("unresolvable")
                .add_lazy_child("unrunnable")
                .offer_runner_to_children(TEST_RUNNER_NAME)
                .build(),
        );
        mock_resolver.add_component("system", component_decl_with_test_runner());
        mock_resolver.add_component("unrunnable", component_decl_with_test_runner());
        let mock_runner = Arc::new(MockRunner::new());
        mock_runner.cause_failure("unrunnable");
        let test =
            RealmCapabilityTest::new(mock_resolver, mock_runner, vec![].into(), vec![]).await;

        // Instance not found.
        {
            let mut child_ref = fsys::ChildRef { name: "missing".to_string(), collection: None };
            let (_, server_end) = endpoints::create_proxy::<DirectoryMarker>().unwrap();
            let err = test
                .realm_proxy
                .bind_child(&mut child_ref, server_end)
                .await
                .expect("fidl call failed")
                .expect_err("unexpected success");
            assert_eq!(err, fcomponent::Error::InstanceNotFound);
        }

        // Instance cannot resolve.
        {
            let mut child_ref =
                fsys::ChildRef { name: "unresolvable".to_string(), collection: None };
            let (_, server_end) = endpoints::create_proxy::<DirectoryMarker>().unwrap();
            let err = test
                .realm_proxy
                .bind_child(&mut child_ref, server_end)
                .await
                .expect("fidl call failed")
                .expect_err("unexpected success");
            assert_eq!(err, fcomponent::Error::InstanceCannotResolve);
        }
    }

    // If a runner fails to launch a child, the error should not occur at `bind_child`.
    #[fuchsia_async::run_singlethreaded(test)]
    async fn bind_child_runner_failure() {
        let mut mock_resolver = MockResolver::new();
        mock_resolver.add_component(
            "root",
            ComponentDeclBuilder::new()
                .add_lazy_child("unrunnable")
                .offer_runner_to_children(TEST_RUNNER_NAME)
                .build(),
        );
        mock_resolver.add_component("unrunnable", component_decl_with_test_runner());
        let mock_runner = Arc::new(MockRunner::new());
        mock_runner.cause_failure("unrunnable");
        let test =
            RealmCapabilityTest::new(mock_resolver, mock_runner, vec![].into(), vec![]).await;

        let mut child_ref = fsys::ChildRef { name: "unrunnable".to_string(), collection: None };
        let (_, server_end) = endpoints::create_proxy::<DirectoryMarker>().unwrap();
        test.realm_proxy
            .bind_child(&mut child_ref, server_end)
            .await
            .expect("fidl call failed")
            .expect("bind failed");
        // TODO(fxb/46913): Assert that `server_end` closes once instance death is monitored.
    }

    fn child_decl(name: &str) -> fsys::ChildDecl {
        ChildDecl {
            name: name.to_string(),
            url: format!("test:///{}", name),
            startup: fsys::StartupMode::Lazy,
            environment: None,
        }
        .native_into_fidl()
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn list_children() {
        // Create a root component with collections and a static child.
        let mut mock_resolver = MockResolver::new();
        mock_resolver.add_component(
            "root",
            ComponentDeclBuilder::new()
                .add_lazy_child("static")
                .add_collection("coll", fsys::Durability::Transient)
                .add_collection("coll2", fsys::Durability::Transient)
                .offer_runner_to_children(TEST_RUNNER_NAME)
                .build(),
        );
        mock_resolver.add_component("static", component_decl_with_test_runner());
        let mock_runner = Arc::new(MockRunner::new());
        let test =
            RealmCapabilityTest::new(mock_resolver, mock_runner, vec![].into(), vec![]).await;

        // Create children "a" and "b" in collection 1, "c" in collection 2.
        let mut collection_ref = fsys::CollectionRef { name: "coll".to_string() };
        let res = test.realm_proxy.create_child(&mut collection_ref, child_decl("a")).await;
        let _ = res.expect("failed to create child a").expect("failed to create child a");

        let mut collection_ref = fsys::CollectionRef { name: "coll".to_string() };
        let res = test.realm_proxy.create_child(&mut collection_ref, child_decl("b")).await;
        let _ = res.expect("failed to create child b").expect("failed to create child b");

        let mut collection_ref = fsys::CollectionRef { name: "coll".to_string() };
        let res = test.realm_proxy.create_child(&mut collection_ref, child_decl("c")).await;
        let _ = res.expect("failed to create child c").expect("failed to create child c");

        let mut collection_ref = fsys::CollectionRef { name: "coll2".to_string() };
        let res = test.realm_proxy.create_child(&mut collection_ref, child_decl("d")).await;
        let _ = res.expect("failed to create child d").expect("failed to create child d");

        // Verify that we see the expected children when listing the collection.
        let (iterator_proxy, server_end) = endpoints::create_proxy().unwrap();
        let mut collection_ref = fsys::CollectionRef { name: "coll".to_string() };
        let res = test.realm_proxy.list_children(&mut collection_ref, server_end).await;
        let _ = res.expect("failed to list children").expect("failed to list children");

        let res = iterator_proxy.next().await;
        let children = res.expect("failed to iterate over children");
        assert_eq!(
            children,
            vec![
                fsys::ChildRef { name: "a".to_string(), collection: Some("coll".to_string()) },
                fsys::ChildRef { name: "b".to_string(), collection: Some("coll".to_string()) },
            ]
        );

        let res = iterator_proxy.next().await;
        let children = res.expect("failed to iterate over children");
        assert_eq!(
            children,
            vec![fsys::ChildRef { name: "c".to_string(), collection: Some("coll".to_string()) },]
        );

        let res = iterator_proxy.next().await;
        let children = res.expect("failed to iterate over children");
        assert_eq!(children, vec![]);
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn list_children_errors() {
        // Create a root component with a collection.
        let mut mock_resolver = MockResolver::new();
        mock_resolver.add_component(
            "root",
            ComponentDeclBuilder::new()
                .add_collection("coll", fsys::Durability::Transient)
                .offer_runner_to_children(TEST_RUNNER_NAME)
                .build(),
        );
        let mock_runner = Arc::new(MockRunner::new());
        let test =
            RealmCapabilityTest::new(mock_resolver, mock_runner, vec![].into(), vec![]).await;

        // Collection not found.
        {
            let mut collection_ref = fsys::CollectionRef { name: "nonexistent".to_string() };
            let (_, server_end) = endpoints::create_proxy().unwrap();
            let err = test
                .realm_proxy
                .list_children(&mut collection_ref, server_end)
                .await
                .expect("fidl call failed")
                .expect_err("unexpected success");
            assert_eq!(err, fcomponent::Error::CollectionNotFound);
        }
    }
}
