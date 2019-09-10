// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::model::*,
    cm_fidl_validator,
    cm_rust::{CapabilityPath, FidlIntoNative, FrameworkCapabilityDecl},
    failure::Error,
    fidl::endpoints::ServerEnd,
    fidl_fuchsia_io::DirectoryMarker,
    fidl_fuchsia_sys2 as fsys, fuchsia_async as fasync, fuchsia_zircon as zx,
    futures::future::BoxFuture,
    futures::prelude::*,
    lazy_static::lazy_static,
    log::*,
    std::{cmp, convert::TryInto, sync::Arc},
};

/// The service-side of a framework capability implements this trait.
/// Multiple FrameworkCapability objects can compose with one another for a single
/// framework capability request. For example, a FrameworkCapabitility can be interposed
/// between the primary FrameworkCapability and the client for the purpose of logging,
/// and testing. A FrameworkCapability is typically provided by a corresponding Hook in
/// response to the on_route_framework_capability event.
pub trait FrameworkCapability: Send + Sync {
    // Called to bind a server end of a zx::Channel to the provided framework capability.
    // If the capability is a directory, then |flags|, |open_mode| and |relative_path|
    // will be propagated along to open the appropriate directory.
    fn open(
        &self,
        flags: u32,
        open_mode: u32,
        relative_path: String,
        server_end: zx::Channel,
    ) -> BoxFuture<Result<(), ModelError>>;
}

lazy_static! {
    pub static ref REALM_SERVICE: CapabilityPath = "/svc/fuchsia.sys2.Realm".try_into().unwrap();
}

// The default implementation for framework services.
pub struct RealmServiceCapability {
    realm: Arc<Realm>,
    host: RealmServiceHost,
}

impl RealmServiceCapability {
    pub fn new(realm: Arc<Realm>, host: RealmServiceHost) -> Self {
        Self { realm, host }
    }

    pub async fn open_async(
        &self,
        _flags: u32,
        _open_mode: u32,
        _relative_path: String,
        server_end: zx::Channel,
    ) -> Result<(), ModelError> {
        let stream = ServerEnd::<fsys::RealmMarker>::new(server_end)
            .into_stream()
            .expect("could not convert channel into stream");
        let realm = self.realm.clone();
        let host = self.host.clone();
        fasync::spawn(async move {
            if let Err(e) = host.serve(realm, stream).await {
                // TODO: Set an epitaph to indicate this was an unexpected error.
                warn!("serve_realm failed: {}", e);
            }
        });
        Ok(())
    }
}

impl FrameworkCapability for RealmServiceCapability {
    fn open(
        &self,
        flags: u32,
        open_mode: u32,
        relative_path: String,
        server_chan: zx::Channel,
    ) -> BoxFuture<Result<(), ModelError>> {
        Box::pin(self.open_async(flags, open_mode, relative_path, server_chan))
    }
}

#[derive(Clone)]
pub struct RealmServiceHost {
    inner: Arc<RealmServiceHostInner>,
}

pub struct RealmServiceHostInner {
    model: Model,
}

// RealmServiceHost is a Hook that injects framework services.
impl RealmServiceHost {
    pub fn new(model: Model) -> Self {
        Self { inner: Arc::new(RealmServiceHostInner::new(model)) }
    }

    pub fn hooks(&self) -> Vec<Hook> {
        // List the hooks the Hub implements here.
        vec![Hook::RouteFrameworkCapability(Arc::new(self.clone()))]
    }

    pub async fn serve(
        &self,
        realm: Arc<Realm>,
        stream: fsys::RealmRequestStream,
    ) -> Result<(), Error> {
        self.inner.serve(realm, stream).await
    }

    pub async fn on_route_framework_capability_async<'a>(
        &'a self,
        realm: Arc<Realm>,
        capability_decl: &'a FrameworkCapabilityDecl,
        capability: Option<Box<dyn FrameworkCapability>>,
    ) -> Result<Option<Box<dyn FrameworkCapability>>, ModelError> {
        // If some other capability has already been installed, then there's nothing to
        // do here.
        match (&capability, capability_decl) {
            (None, FrameworkCapabilityDecl::LegacyService(capability_path))
                if *capability_path == *REALM_SERVICE =>
            {
                return Ok(Some(
                    Box::new(RealmServiceCapability::new(realm.clone(), self.clone()))
                        as Box<dyn FrameworkCapability>,
                ));
            }
            _ => return Ok(capability),
        }
    }
}

impl RouteFrameworkCapabilityHook for RealmServiceHost {
    fn on<'a>(
        &'a self,
        realm: Arc<Realm>,
        capability_decl: &'a FrameworkCapabilityDecl,
        capability: Option<Box<dyn FrameworkCapability>>,
    ) -> BoxFuture<Result<Option<Box<dyn FrameworkCapability>>, ModelError>> {
        Box::pin(self.on_route_framework_capability_async(realm, capability_decl, capability))
    }
}

impl RealmServiceHostInner {
    pub fn new(model: Model) -> Self {
        Self { model }
    }

    async fn serve(
        &self,
        realm: Arc<Realm>,
        mut stream: fsys::RealmRequestStream,
    ) -> Result<(), Error> {
        while let Some(request) = stream.try_next().await? {
            match request {
                fsys::RealmRequest::CreateChild { responder, collection, decl } => {
                    let mut res =
                        Self::create_child(self.model.clone(), realm.clone(), collection, decl)
                            .await;
                    responder.send(&mut res)?;
                }
                fsys::RealmRequest::BindChild { responder, child, exposed_dir } => {
                    let mut res =
                        Self::bind_child(self.model.clone(), realm.clone(), child, exposed_dir)
                            .await;
                    responder.send(&mut res)?;
                }
                fsys::RealmRequest::DestroyChild { responder, child } => {
                    let mut res =
                        Self::destroy_child(self.model.clone(), realm.clone(), child).await;
                    responder.send(&mut res)?;
                }
                fsys::RealmRequest::ListChildren { responder, collection, iter } => {
                    let mut res =
                        Self::list_children(self.model.clone(), realm.clone(), collection, iter)
                            .await;
                    responder.send(&mut res)?;
                }
            }
        }
        Ok(())
    }

    async fn create_child(
        model: Model,
        realm: Arc<Realm>,
        collection: fsys::CollectionRef,
        child_decl: fsys::ChildDecl,
    ) -> Result<(), fsys::Error> {
        cm_fidl_validator::validate_child(&child_decl)
            .map_err(|_| fsys::Error::InvalidArguments)?;
        let child_decl = child_decl.fidl_into_native();
        realm.add_dynamic_child(collection.name, &child_decl, &model.hooks).await.map_err(|e| {
            match e {
                ModelError::InstanceAlreadyExists { .. } => fsys::Error::InstanceAlreadyExists,
                ModelError::CollectionNotFound { .. } => fsys::Error::CollectionNotFound,
                ModelError::Unsupported { .. } => fsys::Error::Unsupported,
                e => {
                    error!("add_dynamic_child() failed: {}", e);
                    fsys::Error::Internal
                }
            }
        })?;
        Ok(())
    }

    async fn bind_child(
        model: Model,
        realm: Arc<Realm>,
        child: fsys::ChildRef,
        exposed_dir: ServerEnd<DirectoryMarker>,
    ) -> Result<(), fsys::Error> {
        let partial_moniker = PartialMoniker::new(child.name, child.collection);
        realm.resolve_decl().await.map_err(|e| match e {
            ModelError::ResolverError { err } => {
                debug!("failed to resolve: {:?}", err);
                fsys::Error::InstanceCannotResolve
            }
            e => {
                error!("resolve_decl() failed: {}", e);
                fsys::Error::Internal
            }
        })?;
        let child_realm = {
            let realm_state = realm.lock_state().await;
            let realm_state = realm_state.get();
            realm_state.get_live_child_realm(&partial_moniker).map(|r| r.clone())
        };
        if let Some(child_realm) = child_realm {
            model
                .bind_instance_open_exposed(child_realm, exposed_dir.into_channel())
                .await
                .map_err(|e| match e {
                    ModelError::ResolverError { err } => {
                        debug!("failed to resolve child: {:?}", err);
                        fsys::Error::InstanceCannotResolve
                    }
                    ModelError::RunnerError { err } => {
                        debug!("failed to start child: {:?}", err);
                        fsys::Error::InstanceCannotStart
                    }
                    e => {
                        error!("bind_instance_open_exposed() failed: {}", e);
                        fsys::Error::Internal
                    }
                })?;
        } else {
            return Err(fsys::Error::InstanceNotFound);
        }
        Ok(())
    }

    async fn destroy_child(
        model: Model,
        realm: Arc<Realm>,
        child: fsys::ChildRef,
    ) -> Result<(), fsys::Error> {
        child.collection.as_ref().ok_or(fsys::Error::InvalidArguments)?;
        let partial_moniker = PartialMoniker::new(child.name, child.collection);
        Realm::remove_dynamic_child(model, realm, &partial_moniker).await.map_err(|e| match e {
            ModelError::InstanceNotFoundInRealm { .. } => fsys::Error::InstanceNotFound,
            ModelError::Unsupported { .. } => fsys::Error::Unsupported,
            e => {
                error!("remove_dynamic_child() failed: {}", e);
                fsys::Error::Internal
            }
        })
    }

    async fn list_children(
        model: Model,
        realm: Arc<Realm>,
        collection: fsys::CollectionRef,
        iter: ServerEnd<fsys::ChildIteratorMarker>,
    ) -> Result<(), fsys::Error> {
        realm.resolve_decl().await.map_err(|e| {
            error!("resolve_decl() failed: {}", e);
            fsys::Error::Internal
        })?;
        let state = realm.lock_state().await;
        let state = state.get();
        let decl = state.decl();
        let _ = decl
            .find_collection(&collection.name)
            .ok_or_else(|| fsys::Error::CollectionNotFound)?;
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
        let batch_size = model.config.list_children_batch_size;
        fasync::spawn(async move {
            if let Err(e) = Self::serve_child_iterator(children, stream, batch_size).await {
                // TODO: Set an epitaph to indicate this was an unexpected error.
                warn!("serve_child_iterator failed: {}", e);
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
}

#[cfg(test)]
mod tests {
    use super::*;
    use {
        crate::{
            model::testing::{mocks::*, routing_test_helpers::*, test_helpers::*, test_hook::*},
            startup,
        },
        cm_rust::{
            self, CapabilityPath, ChildDecl, CollectionDecl, ComponentDecl, ExposeDecl,
            ExposeLegacyServiceDecl, ExposeSource, ExposeTarget, NativeIntoFidl,
        },
        fidl::endpoints,
        fidl_fidl_examples_echo as echo,
        fidl_fuchsia_io::MODE_TYPE_SERVICE,
        fuchsia_async as fasync,
        futures::{channel::mpsc, lock::Mutex},
        io_util::OPEN_RIGHT_READABLE,
        std::collections::HashSet,
        std::convert::TryFrom,
        std::path::PathBuf,
    };

    struct RealmServiceTest {
        realm: Arc<Realm>,
        realm_proxy: fsys::RealmProxy,
    }

    impl RealmServiceTest {
        async fn new(
            mock_resolver: MockResolver,
            mock_runner: MockRunner,
            realm_moniker: AbsoluteMoniker,
            hooks: Vec<Hook>,
        ) -> Self {
            // Init model.
            let mut resolver = ResolverRegistry::new();
            resolver.register("test".to_string(), Box::new(mock_resolver));
            let mut config = ModelConfig::default();
            config.list_children_batch_size = 2;
            let startup_args = startup::Arguments {
                use_builtin_process_launcher: false,
                root_component_url: "".to_string(),
            };
            let model = Model::new(ModelParams {
                root_component_url: "test:///root".to_string(),
                root_resolver_registry: resolver,
                root_default_runner: Arc::new(mock_runner),
                config,
                builtin_services: Arc::new(
                    startup::BuiltinRootServices::new(&startup_args).unwrap(),
                ),
            });
            let realm_service_host = RealmServiceHost::new(model.clone());
            model.hooks.install(realm_service_host.hooks()).await;
            model.hooks.install(hooks).await;

            // Look up and bind to realm.
            let realm = model.look_up_realm(&realm_moniker).await.expect("failed to look up realm");
            model.bind_instance(realm.clone()).await.expect("failed to bind to realm");

            // Host framework service.
            let (realm_proxy, stream) =
                endpoints::create_proxy_and_stream::<fsys::RealmMarker>().unwrap();
            {
                let realm = realm.clone();
                fasync::spawn(async move {
                    realm_service_host
                        .serve(realm, stream)
                        .await
                        .expect("failed serving realm service");
                });
            }
            RealmServiceTest { realm, realm_proxy }
        }
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn create_dynamic_child() {
        // Set up model and realm service.
        let mut mock_resolver = MockResolver::new();
        let mock_runner = MockRunner::new();
        mock_resolver.add_component(
            "root",
            ComponentDecl {
                children: vec![ChildDecl {
                    name: "system".to_string(),
                    url: "test:///system".to_string(),
                    startup: fsys::StartupMode::Lazy,
                }],
                ..default_component_decl()
            },
        );
        mock_resolver.add_component(
            "system",
            ComponentDecl {
                collections: vec![CollectionDecl {
                    name: "coll".to_string(),
                    durability: fsys::Durability::Transient,
                }],
                ..default_component_decl()
            },
        );
        let hook = TestHook::new();
        let test = RealmServiceTest::new(
            mock_resolver,
            mock_runner,
            vec!["system:0"].into(),
            hook.hooks(),
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
        assert_eq!("(system(coll:a,coll:b))", hook.print());
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn create_dynamic_child_errors() {
        let mut mock_resolver = MockResolver::new();
        let mock_runner = MockRunner::new();
        mock_resolver.add_component(
            "root",
            ComponentDecl {
                children: vec![ChildDecl {
                    name: "system".to_string(),
                    url: "test:///system".to_string(),
                    startup: fsys::StartupMode::Lazy,
                }],
                ..default_component_decl()
            },
        );
        mock_resolver.add_component(
            "system",
            ComponentDecl {
                collections: vec![
                    CollectionDecl {
                        name: "coll".to_string(),
                        durability: fsys::Durability::Transient,
                    },
                    CollectionDecl {
                        name: "pcoll".to_string(),
                        durability: fsys::Durability::Persistent,
                    },
                ],
                ..default_component_decl()
            },
        );
        let hook = TestHook::new();
        let test = RealmServiceTest::new(
            mock_resolver,
            mock_runner,
            vec!["system:0"].into(),
            hook.hooks(),
        )
        .await;

        // Invalid arguments.
        {
            let mut collection_ref = fsys::CollectionRef { name: "coll".to_string() };
            let child_decl = fsys::ChildDecl {
                name: Some("a".to_string()),
                url: None,
                startup: Some(fsys::StartupMode::Lazy),
            };
            let err = test
                .realm_proxy
                .create_child(&mut collection_ref, child_decl)
                .await
                .expect("fidl call failed")
                .expect_err("unexpected success");
            assert_eq!(err, fsys::Error::InvalidArguments);
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
            assert_eq!(err, fsys::Error::InstanceAlreadyExists);
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
            assert_eq!(err, fsys::Error::CollectionNotFound);
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
            assert_eq!(err, fsys::Error::Unsupported);
        }
        {
            let mut collection_ref = fsys::CollectionRef { name: "coll".to_string() };
            let child_decl = fsys::ChildDecl {
                name: Some("b".to_string()),
                url: Some("test:///b".to_string()),
                startup: Some(fsys::StartupMode::Eager),
            };
            let err = test
                .realm_proxy
                .create_child(&mut collection_ref, child_decl)
                .await
                .expect("fidl call failed")
                .expect_err("unexpected success");
            assert_eq!(err, fsys::Error::Unsupported);
        }
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn destroy_dynamic_child() {
        // Set up model and realm service.
        let mut mock_resolver = MockResolver::new();
        let mock_runner = MockRunner::new();
        mock_resolver.add_component(
            "root",
            ComponentDecl {
                children: vec![ChildDecl {
                    name: "system".to_string(),
                    url: "test:///system".to_string(),
                    startup: fsys::StartupMode::Lazy,
                }],
                ..default_component_decl()
            },
        );
        mock_resolver.add_component(
            "system",
            ComponentDecl {
                collections: vec![CollectionDecl {
                    name: "coll".to_string(),
                    durability: fsys::Durability::Transient,
                }],
                ..default_component_decl()
            },
        );
        mock_resolver.add_component("a", default_component_decl());
        mock_resolver.add_component("b", default_component_decl());

        let hook = Arc::new(TestHook::new());
        let (destroy_hook, mut stop_send, mut destroy_recv) =
            DestroyHook::new(vec!["system:0", "coll:a:1"].into());
        let mut hooks = vec![];
        hooks.append(&mut hook.hooks());
        hooks.append(&mut DestroyHook::hooks(destroy_hook));
        let test =
            RealmServiceTest::new(mock_resolver, mock_runner, vec!["system:0"].into(), hooks).await;

        // Create children "a" and "b" in collection.
        let mut collection_ref = fsys::CollectionRef { name: "coll".to_string() };
        let res = test.realm_proxy.create_child(&mut collection_ref, child_decl("a")).await;
        let _ = res.expect("failed to create child a").expect("failed to create child a");

        let mut collection_ref = fsys::CollectionRef { name: "coll".to_string() };
        let res = test.realm_proxy.create_child(&mut collection_ref, child_decl("b")).await;
        let _ = res.expect("failed to create child b").expect("failed to create child b");

        let child_realm = get_live_child(&test.realm, "coll:a").await;
        let instance_id = get_instance_id(&test.realm, "coll:a").await;
        assert_eq!("(system(coll:a,coll:b))", hook.print());
        assert_eq!(child_realm.component_url, "test:///a".to_string());
        assert_eq!(instance_id, 1);

        // Destroy "a". "a" is no longer live from the client's perspective, although it's still
        // being destroyed.
        let mut child_ref =
            fsys::ChildRef { name: "a".to_string(), collection: Some("coll".to_string()) };
        let res = test.realm_proxy.destroy_child(&mut child_ref).await;
        let _ = res.expect("failed to destroy child a").expect("failed to destroy child a");

        let actual_children = get_live_children(&test.realm).await;
        let mut expected_children: HashSet<PartialMoniker> = HashSet::new();
        expected_children.insert("coll:b".into());
        assert_eq!(actual_children, expected_children);
        assert_eq!("(system(coll:b))", hook.print());

        // Recreate "a" and verify "a" is back (but it's a different "a"). The old "a" is gone
        // from the client's point of view, but it hasn't been cleaned up yet.
        let mut collection_ref = fsys::CollectionRef { name: "coll".to_string() };
        let child_decl = fsys::ChildDecl {
            name: Some("a".to_string()),
            url: Some("test:///a_alt".to_string()),
            startup: Some(fsys::StartupMode::Lazy),
        };
        let res = test.realm_proxy.create_child(&mut collection_ref, child_decl).await;
        let _ = res.expect("failed to recreate child a").expect("failed to recreate child a");

        assert_eq!("(system(coll:a,coll:b))", hook.print());
        let child_realm = get_live_child(&test.realm, "coll:a").await;
        let instance_id = get_instance_id(&test.realm, "coll:a").await;
        assert_eq!(child_realm.component_url, "test:///a_alt".to_string());
        assert_eq!(instance_id, 3);

        // The destruction of "a" was arrested during `Stop`. The old "a" should still exist,
        // although it's not live.
        assert!(has_child(&test.realm, "coll:a:1").await);
        assert!(has_child(&test.realm, "coll:a:3").await);

        // Finally, let destruction proceed. The old instance of "a" should be cleaned up.
        stop_send.send(()).await.expect("failed to send");
        destroy_recv.next().await.expect("failed to receive");
        assert!(!has_child(&test.realm, "coll:a:1").await);
        assert!(has_child(&test.realm, "coll:a:3").await);
    }

    struct DestroyHook {
        /// Realm for which to block `on_stop_instance`.
        moniker: AbsoluteMoniker,
        /// Receiver on which to wait to unblock `on_stop_instance`.
        stop_recv: Mutex<mpsc::Receiver<()>>,
        /// Receiver on which `on_destroy_instance` is signalled.
        destroy_send: Mutex<mpsc::Sender<()>>,
    }

    impl DestroyHook {
        /// Returns `DestroyHook` and channels on which to signal on `on_stop_instance` and
        /// be signalled for `on_destroy_instance`.
        fn new(moniker: AbsoluteMoniker) -> (Arc<Self>, mpsc::Sender<()>, mpsc::Receiver<()>) {
            let (stop_send, stop_recv) = mpsc::channel(0);
            let (destroy_send, destroy_recv) = mpsc::channel(0);
            (
                Arc::new(Self {
                    moniker,
                    stop_recv: Mutex::new(stop_recv),
                    destroy_send: Mutex::new(destroy_send),
                }),
                stop_send,
                destroy_recv,
            )
        }

        async fn on_stop_instance_async(&self, realm: Arc<Realm>) -> Result<(), ModelError> {
            if realm.abs_moniker == self.moniker {
                let mut recv = self.stop_recv.lock().await;
                recv.next().await.expect("failed to suspend stop");
            }
            Ok(())
        }

        async fn on_destroy_instance_async(&self, realm: Arc<Realm>) -> Result<(), ModelError> {
            if realm.abs_moniker == self.moniker {
                let mut send = self.destroy_send.lock().await;
                send.send(()).await.expect("failed to send destroy signal");
            }
            Ok(())
        }

        fn hooks(hook: Arc<DestroyHook>) -> Vec<Hook> {
            vec![Hook::StopInstance(hook.clone()), Hook::DestroyInstance(hook.clone())]
        }
    }

    impl DestroyInstanceHook for DestroyHook {
        fn on(&self, realm: Arc<Realm>) -> BoxFuture<Result<(), ModelError>> {
            Box::pin(self.on_destroy_instance_async(realm))
        }
    }

    impl StopInstanceHook for DestroyHook {
        fn on(&self, realm: Arc<Realm>) -> BoxFuture<Result<(), ModelError>> {
            Box::pin(self.on_stop_instance_async(realm))
        }
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn destroy_dynamic_child_errors() {
        let mut mock_resolver = MockResolver::new();
        let mock_runner = MockRunner::new();
        mock_resolver.add_component(
            "root",
            ComponentDecl {
                children: vec![ChildDecl {
                    name: "system".to_string(),
                    url: "test:///system".to_string(),
                    startup: fsys::StartupMode::Lazy,
                }],
                ..default_component_decl()
            },
        );
        mock_resolver.add_component(
            "system",
            ComponentDecl {
                collections: vec![CollectionDecl {
                    name: "coll".to_string(),
                    durability: fsys::Durability::Transient,
                }],
                ..default_component_decl()
            },
        );
        let hook = TestHook::new();
        let test = RealmServiceTest::new(
            mock_resolver,
            mock_runner,
            vec!["system:0"].into(),
            hook.hooks(),
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
            assert_eq!(err, fsys::Error::InvalidArguments);
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
            assert_eq!(err, fsys::Error::InstanceNotFound);
        }
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn bind_static_child() {
        // Create a hierarchy of three components, the last with eager startup. The middle
        // component hosts and exposes the "/svc/hippo" service.
        let mut mock_resolver = MockResolver::new();
        mock_resolver.add_component(
            "root",
            ComponentDecl {
                children: vec![ChildDecl {
                    name: "system".to_string(),
                    url: "test:///system".to_string(),
                    startup: fsys::StartupMode::Lazy,
                }],
                ..default_component_decl()
            },
        );
        mock_resolver.add_component(
            "system",
            ComponentDecl {
                exposes: vec![ExposeDecl::LegacyService(ExposeLegacyServiceDecl {
                    source: ExposeSource::Self_,
                    source_path: CapabilityPath::try_from("/svc/foo").unwrap(),
                    target_path: CapabilityPath::try_from("/svc/hippo").unwrap(),
                    target: ExposeTarget::Realm,
                })],
                children: vec![ChildDecl {
                    name: "eager".to_string(),
                    url: "test:///eager".to_string(),
                    startup: fsys::StartupMode::Eager,
                }],
                ..default_component_decl()
            },
        );
        mock_resolver.add_component("eager", ComponentDecl { ..default_component_decl() });
        let mut mock_runner = MockRunner::new();
        let mut out_dir = OutDir::new();
        out_dir.add_service();
        mock_runner.host_fns.insert("test:///system_resolved".to_string(), out_dir.host_fn());
        let urls_run = mock_runner.urls_run.clone();
        let hook = TestHook::new();
        let test =
            RealmServiceTest::new(mock_resolver, mock_runner, vec![].into(), hook.hooks()).await;

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
        assert_eq!(*urls_run.lock().await, expected_urls);
        assert_eq!("(system(eager))", hook.print());
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn bind_dynamic_child() {
        // Create a root component with a collection and define a component that exposes a service.
        let mut mock_resolver = MockResolver::new();
        mock_resolver.add_component(
            "root",
            ComponentDecl {
                collections: vec![CollectionDecl {
                    name: "coll".to_string(),
                    durability: fsys::Durability::Transient,
                }],
                ..default_component_decl()
            },
        );
        mock_resolver.add_component(
            "system",
            ComponentDecl {
                exposes: vec![ExposeDecl::LegacyService(ExposeLegacyServiceDecl {
                    source: ExposeSource::Self_,
                    source_path: CapabilityPath::try_from("/svc/foo").unwrap(),
                    target_path: CapabilityPath::try_from("/svc/hippo").unwrap(),
                    target: ExposeTarget::Realm,
                })],
                ..default_component_decl()
            },
        );
        let mut mock_runner = MockRunner::new();
        let mut out_dir = OutDir::new();
        out_dir.add_service();
        mock_runner.host_fns.insert("test:///system_resolved".to_string(), out_dir.host_fn());
        let urls_run = mock_runner.urls_run.clone();
        let hook = TestHook::new();
        let test =
            RealmServiceTest::new(mock_resolver, mock_runner, vec![].into(), hook.hooks()).await;

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
        assert_eq!(*urls_run.lock().await, expected_urls);
        assert_eq!("(coll:system)", hook.print());
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn bind_child_errors() {
        let mut mock_resolver = MockResolver::new();
        mock_resolver.add_component(
            "root",
            ComponentDecl {
                children: vec![
                    ChildDecl {
                        name: "system".to_string(),
                        url: "test:///system".to_string(),
                        startup: fsys::StartupMode::Lazy,
                    },
                    ChildDecl {
                        name: "unresolvable".to_string(),
                        url: "test:///unresolvable".to_string(),
                        startup: fsys::StartupMode::Lazy,
                    },
                    ChildDecl {
                        name: "unrunnable".to_string(),
                        url: "test:///unrunnable".to_string(),
                        startup: fsys::StartupMode::Lazy,
                    },
                ],
                ..default_component_decl()
            },
        );
        mock_resolver.add_component("system", ComponentDecl { ..default_component_decl() });
        mock_resolver.add_component("unrunnable", ComponentDecl { ..default_component_decl() });
        let mut mock_runner = MockRunner::new();
        mock_runner.cause_failure("unrunnable");
        let hook = TestHook::new();
        let test =
            RealmServiceTest::new(mock_resolver, mock_runner, vec![].into(), hook.hooks()).await;

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
            assert_eq!(err, fsys::Error::InstanceNotFound);
        }

        // Instance cannot start.
        {
            let mut child_ref = fsys::ChildRef { name: "unrunnable".to_string(), collection: None };
            let (_, server_end) = endpoints::create_proxy::<DirectoryMarker>().unwrap();
            let err = test
                .realm_proxy
                .bind_child(&mut child_ref, server_end)
                .await
                .expect("fidl call failed")
                .expect_err("unexpected success");
            assert_eq!(err, fsys::Error::InstanceCannotStart);
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
            assert_eq!(err, fsys::Error::InstanceCannotResolve);
        }
    }

    fn child_decl(name: &str) -> fsys::ChildDecl {
        ChildDecl {
            name: name.to_string(),
            url: format!("test:///{}", name),
            startup: fsys::StartupMode::Lazy,
        }
        .native_into_fidl()
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn list_children() {
        // Create a root component with collections and a static child.
        let mut mock_resolver = MockResolver::new();
        mock_resolver.add_component(
            "root",
            ComponentDecl {
                children: vec![ChildDecl {
                    name: "static".to_string(),
                    url: "test:///static".to_string(),
                    startup: fsys::StartupMode::Lazy,
                }],
                collections: vec![
                    CollectionDecl {
                        name: "coll".to_string(),
                        durability: fsys::Durability::Transient,
                    },
                    CollectionDecl {
                        name: "coll2".to_string(),
                        durability: fsys::Durability::Transient,
                    },
                ],
                ..default_component_decl()
            },
        );
        mock_resolver.add_component("static", default_component_decl());
        let mock_runner = MockRunner::new();
        let hook = TestHook::new();
        let test =
            RealmServiceTest::new(mock_resolver, mock_runner, vec![].into(), hook.hooks()).await;

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
            ComponentDecl {
                collections: vec![CollectionDecl {
                    name: "coll".to_string(),
                    durability: fsys::Durability::Transient,
                }],
                ..default_component_decl()
            },
        );
        let mock_runner = MockRunner::new();
        let hook = TestHook::new();
        let test =
            RealmServiceTest::new(mock_resolver, mock_runner, vec![].into(), hook.hooks()).await;

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
            assert_eq!(err, fsys::Error::CollectionNotFound);
        }
    }
}
