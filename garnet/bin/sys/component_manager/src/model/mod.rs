// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

mod moniker;
mod resolver;
mod runner;

pub use self::{moniker::*, resolver::*, runner::*};
use {
    crate::ns_util::PKG_PATH,
    crate::{data, io_util},
    failure::{Error, Fail},
    fidl::endpoints::{ClientEnd, Proxy, ServerEnd},
    fidl_fuchsia_io::DirectoryProxy,
    fidl_fuchsia_sys2 as fsys, fuchsia_async as fasync, fuchsia_zircon as zx,
    futures::lock::Mutex,
    std::{collections::HashMap, sync::Arc},
};

/// Parameters for initializing a component model, particularly the root of the component
/// instance tree.
pub struct ModelParams {
    /// The URI of the root component.
    pub root_component_uri: String,
    /// The component resolver registry used in the root realm.
    /// In particular, it will be used to resolve the root component itself.
    pub root_resolver_registry: ResolverRegistry,
    /// The default runner used in the root realm (nominally runs ELF binaries).
    pub root_default_runner: Box<dyn Runner + Send + Sync + 'static>,
}

/// The component model holds authoritative state about a tree of component instances, including
/// each instance's identity, lifecycle, capabilities, and topological relationships.  It also
/// provides operations for instantiating, destroying, querying, and controlling component
/// instances at runtime.
///
/// To facilitate unit testing, the component model does not directly perform IPC.  Instead, it
/// delegates external interfacing concerns to other objects that implement traits such as
/// `Runner` and `Resolver`.
pub struct Model {
    root_realm: Arc<Mutex<Realm>>,
}

/// A realm is a container for an individual component instance and its children.  It is provided
/// by the parent of the instance or by the component manager itself in the case of the root realm.
///
/// The realm's properties influence the runtime behavior of the subtree of component instances
/// that it contains, including component resolution, execution, and service discovery.
type ChildRealmMap = HashMap<ChildMoniker, Arc<Mutex<Realm>>>;
struct Realm {
    /// The registry for resolving component URIs within the realm.
    resolver_registry: Arc<ResolverRegistry>,
    /// The default runner (nominally runs ELF binaries) for executing components
    /// within the realm that do not explicitly specify a runner.
    default_runner: Arc<Box<dyn Runner + Send + Sync + 'static>>,
    /// The component that has been instantiated within the realm.
    instance: Instance,
}

/// An instance of a component.
struct Instance {
    /// The component's URI.
    component_uri: String,
    /// Execution state for the component instance or `None` if not running.
    execution: Mutex<Option<Execution>>,
    /// Realms of child instances, indexed by child moniker (name). Evaluated on demand.
    child_realms: Option<ChildRealmMap>,
    /// The mode of startup (lazy or eager).
    startup: fsys::StartupMode,
}

impl Instance {
    fn make_child_realms(
        component: &fsys::Component,
        resolver_registry: Arc<ResolverRegistry>,
        default_runner: Arc<Box<dyn Runner + Send + Sync + 'static>>,
    ) -> Result<ChildRealmMap, ModelError> {
        let mut child_realms = HashMap::new();
        if component.decl.is_none() {
            return Err(ModelError::ComponentInvalid);
        }
        if let Some(ref children_decl) = component.decl.as_ref().unwrap().children {
            for child_decl in children_decl {
                let child_name = child_decl.name.as_ref().unwrap().clone();
                let child_uri = child_decl.uri.as_ref().unwrap().clone();
                let moniker = ChildMoniker::new(child_name);
                let realm = Arc::new(Mutex::new(Realm {
                    resolver_registry: resolver_registry.clone(),
                    default_runner: default_runner.clone(),
                    instance: Instance {
                        component_uri: child_uri,
                        execution: Mutex::new(None),
                        child_realms: None,
                        startup: child_decl.startup.unwrap(),
                    },
                }));
                child_realms.insert(moniker, realm);
            }
        }
        Ok(child_realms)
    }
}

/// The execution state for a component instance that has started running.
// TODO: Hold the component instance's controller.
struct Execution {
    resolved_uri: String,
    decl: fsys::ComponentDecl,
    package_dir: Option<DirectoryProxy>,
    outgoing_dir: DirectoryProxy,
}

impl Execution {
    fn start_from(
        component: fsys::Component,
        outgoing_dir: DirectoryProxy,
    ) -> Result<Self, ModelError> {
        if component.resolved_uri.is_none() || component.decl.is_none() {
            return Err(ModelError::ComponentInvalid);
        }
        let package_dir = match component.package {
            Some(package) => {
                if package.package_dir.is_none() {
                    return Err(ModelError::ComponentInvalid);
                }
                let package_dir = package
                    .package_dir
                    .unwrap()
                    .into_proxy()
                    .expect("could not convert package dir to proxy");
                Some(package_dir)
            }
            None => None,
        };
        Ok(Execution {
            resolved_uri: component.resolved_uri.unwrap(),
            decl: component.decl.unwrap(),
            package_dir,
            outgoing_dir,
        })
    }
}

type UseDeclsOpt = std::option::Option<std::vec::Vec<fidl_fuchsia_sys2::UseDecl>>;

impl Execution {
    async fn make_namespace<'a>(
        &'a self,
        uses: &'a UseDeclsOpt,
    ) -> Result<fsys::ComponentNamespace, ModelError> {
        // TODO: Populate namespace from the component declaration.
        let mut ns = fsys::ComponentNamespace { paths: vec![], directories: vec![] };

        // Populate the namespace from uses, using the component manager's namespace.
        if let Some(uses) = uses {
            for use_ in uses {
                match use_.type_ {
                    Some(fsys::CapabilityType::Directory) => {
                        let source_path = &use_.source_path.as_ref().unwrap();
                        let dir_proxy = io_util::open_directory_in_namespace(&source_path)
                            .map_err(|e| ModelError::namespace_creation_failed(e))?;
                        let dir =
                            ClientEnd::new(dir_proxy.into_channel().unwrap().into_zx_channel());
                        ns.paths.push(use_.target_path.as_ref().unwrap().to_string());
                        ns.directories.push(dir);
                    }
                    Some(fsys::CapabilityType::Service) => {
                        // TODO(CF-583): support services
                    }
                    None => {
                        return Err(ModelError::namespace_parsing_failed(
                            "UseDecl missing CapabilityType".to_string(),
                        ));
                    }
                }
            }
        }

        // Populate the /pkg namespace.
        if let Some(package_dir) = self.package_dir.as_ref() {
            let clone_dir_proxy = io_util::clone_directory(package_dir)
                .map_err(|e| ModelError::namespace_creation_failed(e))?;
            let cloned_dir = ClientEnd::new(
                clone_dir_proxy
                    .into_channel()
                    .expect("could not convert directory to channel")
                    .into_zx_channel(),
            );
            ns.paths.push(PKG_PATH.to_str().unwrap().to_string());
            ns.directories.push(cloned_dir);
        }
        Ok(ns)
    }
}

impl Model {
    /// Creates a new component model and initializes its topology.
    pub fn new(params: ModelParams) -> Model {
        Model {
            root_realm: Arc::new(Mutex::new(Realm {
                resolver_registry: Arc::new(params.root_resolver_registry),
                default_runner: Arc::new(params.root_default_runner),
                instance: Instance {
                    component_uri: params.root_component_uri,
                    execution: Mutex::new(None),
                    child_realms: None,
                    // Started by main().
                    startup: fsys::StartupMode::Lazy,
                },
            })),
        }
    }

    /// Binds to the component instance with the specified moniker, causing it to start if it is
    /// not already running. Also binds to any descendant component instances that need to be
    /// eagerly started.
    pub async fn bind_instance(&self, abs_moniker: AbsoluteMoniker) -> Result<(), ModelError> {
        let realm = await!(self.look_up_realm(abs_moniker))?;
        // We may have to bind to multiple instances if this instance has children with the
        // "eager" startup mode.
        let mut instances_to_bind = vec![realm];
        while let Some(realm) = instances_to_bind.pop() {
            instances_to_bind.append(&mut await!(self.bind_instance_in_realm(realm))?);
        }
        Ok(())
    }

    /// Binds to the component instance in the given realm, starting it if it's not
    /// already running. Returns the list of child realms whose instances need to be eagerly started
    /// after this function returns.
    async fn bind_instance_in_realm(
        &self,
        realm_cell: Arc<Mutex<Realm>>,
    ) -> Result<Vec<Arc<Mutex<Realm>>>, ModelError> {
        // There can only be one task manipulating an instance's execution at a time.
        let Realm { ref resolver_registry, ref default_runner, ref mut instance } =
            *await!(realm_cell.lock());
        let Instance { ref component_uri, ref execution, ref mut child_realms, startup: _ } =
            instance;
        let mut execution_lock = await!(execution.lock());
        let mut started = false;
        match &*execution_lock {
            Some(_) => {}
            None => {
                let component = await!(resolver_registry.resolve(component_uri))?;
                if child_realms.is_none() {
                    *child_realms = Some(Instance::make_child_realms(
                        &component,
                        resolver_registry.clone(),
                        default_runner.clone(),
                    )?);
                }
                // TODO(CF-647): Serve in the Instance' PseudoDir instead.
                let (outgoing_dir_client, outgoing_dir_server) =
                    zx::Channel::create().map_err(|e| ModelError::namespace_creation_failed(e))?;
                let execution = Execution::start_from(
                    component,
                    DirectoryProxy::from_channel(
                        fasync::Channel::from_channel(outgoing_dir_client).unwrap(),
                    ),
                )?;

                let ns = await!(execution.make_namespace(&execution.decl.uses))?;

                let start_info = fsys::ComponentStartInfo {
                    resolved_uri: Some(execution.resolved_uri.clone()),
                    program: data::clone_option_dictionary(&execution.decl.program),
                    ns: Some(ns),
                    outgoing_dir: Some(ServerEnd::new(outgoing_dir_server)),
                };
                await!(default_runner.start(start_info))?;
                started = true;
                *execution_lock = Some(execution);
            }
        }
        // Return children that need eager starting.
        let mut eager_children = vec![];
        if started {
            for child_realm in child_realms.as_ref().unwrap().values() {
                let startup = await!(child_realm.lock()).instance.startup;
                match startup {
                    fsys::StartupMode::Eager => {
                        eager_children.push(child_realm.clone());
                    }
                    fsys::StartupMode::Lazy => {}
                }
            }
        }
        Ok(eager_children)
    }

    async fn look_up_realm(
        &self,
        abs_moniker: AbsoluteMoniker,
    ) -> Result<Arc<Mutex<Realm>>, ModelError> {
        let mut cur_realm = self.root_realm.clone();
        for moniker in abs_moniker.path().iter() {
            cur_realm = {
                let Realm { ref resolver_registry, ref default_runner, ref mut instance } =
                    *await!(cur_realm.lock());
                if instance.child_realms.is_none() {
                    let component = await!(resolver_registry.resolve(&instance.component_uri))?;
                    instance.child_realms = Some(Instance::make_child_realms(
                        &component,
                        resolver_registry.clone(),
                        default_runner.clone(),
                    )?);
                }
                let child_realms = instance.child_realms.as_ref().unwrap();
                if !child_realms.contains_key(&moniker) {
                    return Err(ModelError::instance_not_found(abs_moniker));
                }
                child_realms[moniker].clone()
            }
        }
        Ok(cur_realm)
    }
}

// TODO: Derive from Fail and take cause where appropriate.
/// Errors produced by `Model`.
#[derive(Debug, Fail)]
pub enum ModelError {
    #[fail(display = "component instance not found with moniker {}", moniker)]
    InstanceNotFound { moniker: AbsoluteMoniker },
    #[fail(display = "component declaration invalid")]
    ComponentInvalid,
    #[fail(display = "namespace creation failed: {}", err)]
    NamespaceCreationFailed {
        #[fail(cause)]
        err: Error,
    },
    #[fail(display = "namespace parsing failed: {}", s)]
    NamespaceParsingFailed { s: String },
    #[fail(display = "resolver error")]
    ResolverError {
        #[fail(cause)]
        err: ResolverError,
    },
    #[fail(display = "runner error")]
    RunnerError {
        #[fail(cause)]
        err: RunnerError,
    },
}

impl ModelError {
    fn instance_not_found(moniker: AbsoluteMoniker) -> ModelError {
        ModelError::InstanceNotFound { moniker }
    }

    fn namespace_creation_failed(err: impl Into<Error>) -> ModelError {
        ModelError::NamespaceCreationFailed { err: err.into() }
    }

    fn namespace_parsing_failed(s: String) -> ModelError {
        ModelError::NamespaceParsingFailed { s: s }
    }
}

impl From<ResolverError> for ModelError {
    fn from(err: ResolverError) -> Self {
        ModelError::ResolverError { err }
    }
}

impl From<RunnerError> for ModelError {
    fn from(err: RunnerError) -> Self {
        ModelError::RunnerError { err }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use fidl_fuchsia_io::DirectoryMarker;
    use fidl_fuchsia_sys2::{CapabilityType, ComponentNamespace, UseDecl};
    use futures::future::FutureObj;
    use lazy_static::lazy_static;
    use regex::Regex;
    use std::collections::HashSet;
    use std::path::PathBuf;

    struct ChildInfo {
        name: String,
        startup: fsys::StartupMode,
    }

    struct MockResolver {
        // Map of parent component instance to its children component instances.
        children: Arc<Mutex<HashMap<String, Vec<ChildInfo>>>>,
        // Map of component instance to vec of UseDecls of the component instance.
        uses: Arc<Mutex<HashMap<String, Option<Vec<UseDecl>>>>>,
    }
    lazy_static! {
        static ref NAME_RE: Regex = Regex::new(r"test:///([0-9a-z\-\._]+)$").unwrap();
    }
    impl MockResolver {
        async fn resolve_async(
            &self,
            component_uri: String,
        ) -> Result<fsys::Component, ResolverError> {
            let caps = NAME_RE.captures(&component_uri).unwrap();
            let name = &caps[1];
            let children = await!(self.children.lock());
            let mut decl = new_component_decl();
            if let Some(children) = children.get(name) {
                decl.children = Some(
                    children
                        .iter()
                        .map(|c| fsys::ChildDecl {
                            name: Some(c.name.clone()),
                            uri: Some(format!("test:///{}", c.name)),
                            startup: Some(c.startup),
                        })
                        .collect(),
                );
            }
            let mut uses = await!(self.uses.lock());
            if let Some(uses) = uses.remove(name) {
                decl.uses = uses;
            }
            Ok(fsys::Component {
                resolved_uri: Some(format!("test:///{}_resolved", name)),
                decl: Some(decl),
                package: None,
            })
        }
    }
    impl Resolver for MockResolver {
        fn resolve(
            &self,
            component_uri: &str,
        ) -> FutureObj<Result<fsys::Component, ResolverError>> {
            FutureObj::new(Box::new(self.resolve_async(component_uri.to_string())))
        }
    }
    struct MockRunner {
        uris_run: Arc<Mutex<Vec<String>>>,
        namespaces: Arc<Mutex<HashMap<String, ComponentNamespace>>>,
        outgoing_dirs: Arc<Mutex<HashMap<String, ServerEnd<DirectoryMarker>>>>,
    }
    impl MockRunner {
        async fn start_async(
            &self,
            start_info: fsys::ComponentStartInfo,
        ) -> Result<(), RunnerError> {
            let resolved_uri = start_info.resolved_uri.unwrap();
            await!(self.uris_run.lock()).push(resolved_uri.clone());
            await!(self.namespaces.lock()).insert(resolved_uri.clone(), start_info.ns.unwrap());
            await!(self.outgoing_dirs.lock())
                .insert(resolved_uri.clone(), start_info.outgoing_dir.unwrap());
            Ok(())
        }
    }
    impl Runner for MockRunner {
        fn start(
            &self,
            start_info: fsys::ComponentStartInfo,
        ) -> FutureObj<Result<(), RunnerError>> {
            FutureObj::new(Box::new(self.start_async(start_info)))
        }
    }
    fn new_component_decl() -> fsys::ComponentDecl {
        fsys::ComponentDecl {
            program: None,
            uses: None,
            exposes: None,
            offers: None,
            facets: None,
            children: None,
        }
    }
    #[fuchsia_async::run_singlethreaded(test)]
    async fn bind_instance_root() {
        let mut resolver = ResolverRegistry::new();
        let uris_run = Arc::new(Mutex::new(vec![]));
        let namespaces = Arc::new(Mutex::new(HashMap::new()));
        let outgoing_dirs = Arc::new(Mutex::new(HashMap::new()));
        let runner = MockRunner {
            uris_run: uris_run.clone(),
            namespaces: namespaces.clone(),
            outgoing_dirs: outgoing_dirs.clone(),
        };
        let children = Arc::new(Mutex::new(HashMap::new()));
        let uses = Arc::new(Mutex::new(HashMap::new()));
        resolver.register(
            "test".to_string(),
            Box::new(MockResolver { children: children.clone(), uses: uses.clone() }),
        );
        let model = Model::new(ModelParams {
            root_component_uri: "test:///root".to_string(),
            root_resolver_registry: resolver,
            root_default_runner: Box::new(runner),
        });
        let res = await!(model.bind_instance(AbsoluteMoniker::root()));
        let expected_res: Result<(), ModelError> = Ok(());
        assert_eq!(format!("{:?}", res), format!("{:?}", expected_res));
        let actual_uris = await!(uris_run.lock());
        let expected_uris: Vec<String> = vec!["test:///root_resolved".to_string()];
        assert_eq!(*actual_uris, expected_uris);
        let root_realm = await!(model.root_realm.lock());
        let actual_children = get_children(&root_realm);
        assert!(actual_children.is_empty());
    }
    #[fuchsia_async::run_singlethreaded(test)]
    async fn bind_instance_root_non_existent() {
        let mut resolver = ResolverRegistry::new();
        let uris_run = Arc::new(Mutex::new(vec![]));
        let namespaces = Arc::new(Mutex::new(HashMap::new()));
        let outgoing_dirs = Arc::new(Mutex::new(HashMap::new()));
        let runner = MockRunner {
            uris_run: uris_run.clone(),
            namespaces: namespaces.clone(),
            outgoing_dirs: outgoing_dirs.clone(),
        };
        let children = Arc::new(Mutex::new(HashMap::new()));
        let uses = Arc::new(Mutex::new(HashMap::new()));
        resolver.register(
            "test".to_string(),
            Box::new(MockResolver { children: children.clone(), uses: uses.clone() }),
        );
        let model = Model::new(ModelParams {
            root_component_uri: "test:///root".to_string(),
            root_resolver_registry: resolver,
            root_default_runner: Box::new(runner),
        });
        let res = await!(model.bind_instance(AbsoluteMoniker::new(vec![ChildMoniker::new(
            "no-such-instance".to_string()
        )])));
        let expected_res: Result<(), ModelError> = Err(ModelError::instance_not_found(
            AbsoluteMoniker::new(vec![ChildMoniker::new("no-such-instance".to_string())]),
        ));
        assert_eq!(format!("{:?}", res), format!("{:?}", expected_res));
        let actual_uris = await!(uris_run.lock());
        let expected_uris: Vec<String> = vec![];
        assert_eq!(*actual_uris, expected_uris);
    }
    #[fuchsia_async::run_singlethreaded(test)]
    async fn bind_instance_child() {
        let mut resolver = ResolverRegistry::new();
        let uris_run = Arc::new(Mutex::new(vec![]));
        let namespaces = Arc::new(Mutex::new(HashMap::new()));
        let outgoing_dirs = Arc::new(Mutex::new(HashMap::new()));
        let runner = MockRunner {
            uris_run: uris_run.clone(),
            namespaces: namespaces.clone(),
            outgoing_dirs: outgoing_dirs.clone(),
        };
        let children = Arc::new(Mutex::new(HashMap::new()));
        await!(children.lock()).insert(
            "root".to_string(),
            vec![
                ChildInfo { name: "system".to_string(), startup: fsys::StartupMode::Lazy },
                ChildInfo { name: "echo".to_string(), startup: fsys::StartupMode::Lazy },
            ],
        );
        let uses = Arc::new(Mutex::new(HashMap::new()));
        resolver.register(
            "test".to_string(),
            Box::new(MockResolver { children: children.clone(), uses: uses.clone() }),
        );
        let model = Model::new(ModelParams {
            root_component_uri: "test:///root".to_string(),
            root_resolver_registry: resolver,
            root_default_runner: Box::new(runner),
        });
        // bind to system
        {
            let res = await!(model.bind_instance(AbsoluteMoniker::new(vec![ChildMoniker::new(
                "system".to_string()
            ),])));
            let expected_res: Result<(), ModelError> = Ok(());
            assert_eq!(format!("{:?}", res), format!("{:?}", expected_res));
            let actual_uris = await!(uris_run.lock());
            let expected_uris: Vec<String> = vec!["test:///system_resolved".to_string()];
            assert_eq!(*actual_uris, expected_uris);
        }
        // Validate children. system is resolved, but not echo.
        {
            let actual_children = get_children(&*await!(model.root_realm.lock()));
            let mut expected_children: HashSet<ChildMoniker> = HashSet::new();
            expected_children.insert(ChildMoniker::new("system".to_string()));
            expected_children.insert(ChildMoniker::new("echo".to_string()));
            assert_eq!(actual_children, expected_children);
        }
        {
            let system_realm = get_child_realm(&*await!(model.root_realm.lock()), "system");
            let echo_realm = get_child_realm(&*await!(model.root_realm.lock()), "echo");
            let actual_children = get_children(&*await!(system_realm.lock()));
            assert!(actual_children.is_empty());
            assert!(await!(echo_realm.lock()).instance.child_realms.is_none());
        }
        // bind to echo
        {
            let res = await!(model
                .bind_instance(AbsoluteMoniker::new(vec![ChildMoniker::new("echo".to_string()),])));
            let expected_res: Result<(), ModelError> = Ok(());
            assert_eq!(format!("{:?}", res), format!("{:?}", expected_res));
            let actual_uris = await!(uris_run.lock());
            let expected_uris: Vec<String> =
                vec!["test:///system_resolved".to_string(), "test:///echo_resolved".to_string()];
            assert_eq!(*actual_uris, expected_uris);
        }
        // Validate children. Now echo is resolved.
        {
            let echo_realm = get_child_realm(&*await!(model.root_realm.lock()), "echo");
            let actual_children = get_children(&*await!(echo_realm.lock()));
            assert!(actual_children.is_empty());
        }
    }
    #[fuchsia_async::run_singlethreaded(test)]
    async fn bind_instance_child_non_existent() {
        let mut resolver = ResolverRegistry::new();
        let uris_run = Arc::new(Mutex::new(vec![]));
        let namespaces = Arc::new(Mutex::new(HashMap::new()));
        let outgoing_dirs = Arc::new(Mutex::new(HashMap::new()));
        let runner = MockRunner {
            uris_run: uris_run.clone(),
            namespaces: namespaces.clone(),
            outgoing_dirs: outgoing_dirs.clone(),
        };
        let children = Arc::new(Mutex::new(HashMap::new()));
        await!(children.lock()).insert(
            "root".to_string(),
            vec![ChildInfo { name: "system".to_string(), startup: fsys::StartupMode::Lazy }],
        );
        let uses = Arc::new(Mutex::new(HashMap::new()));
        resolver.register(
            "test".to_string(),
            Box::new(MockResolver { children: children.clone(), uses: uses.clone() }),
        );
        let model = Model::new(ModelParams {
            root_component_uri: "test:///root".to_string(),
            root_resolver_registry: resolver,
            root_default_runner: Box::new(runner),
        });
        // bind to system
        {
            let res = await!(model.bind_instance(AbsoluteMoniker::new(vec![ChildMoniker::new(
                "system".to_string()
            ),])));
            let expected_res: Result<(), ModelError> = Ok(());
            assert_eq!(format!("{:?}", res), format!("{:?}", expected_res));
            let actual_uris = await!(uris_run.lock());
            let expected_uris: Vec<String> = vec!["test:///system_resolved".to_string()];
            assert_eq!(*actual_uris, expected_uris);
        }
        // can't bind to logger: it does not exist
        {
            let moniker = AbsoluteMoniker::new(vec![
                ChildMoniker::new("system".to_string()),
                ChildMoniker::new("logger".to_string()),
            ]);
            let res = await!(model.bind_instance(moniker.clone()));
            let expected_res: Result<(), ModelError> = Err(ModelError::instance_not_found(moniker));
            assert_eq!(format!("{:?}", res), format!("{:?}", expected_res));
            let actual_uris = await!(uris_run.lock());
            let expected_uris: Vec<String> = vec!["test:///system_resolved".to_string()];
            assert_eq!(*actual_uris, expected_uris);
        }
    }
    #[fuchsia_async::run_singlethreaded(test)]
    async fn bind_instance_eager_child() {
        // Create a hierarchy of children. The two components in the middle enable eager binding.
        let mut resolver = ResolverRegistry::new();
        let uris_run = Arc::new(Mutex::new(vec![]));
        let namespaces = Arc::new(Mutex::new(HashMap::new()));
        let outgoing_dirs = Arc::new(Mutex::new(HashMap::new()));
        let runner = MockRunner {
            uris_run: uris_run.clone(),
            namespaces: namespaces.clone(),
            outgoing_dirs: outgoing_dirs.clone(),
        };
        let children = Arc::new(Mutex::new(HashMap::new()));
        await!(children.lock()).insert(
            "root".to_string(),
            vec![ChildInfo { name: "a".to_string(), startup: fsys::StartupMode::Lazy }],
        );
        await!(children.lock()).insert(
            "a".to_string(),
            vec![ChildInfo { name: "b".to_string(), startup: fsys::StartupMode::Eager }],
        );
        await!(children.lock()).insert(
            "b".to_string(),
            vec![ChildInfo { name: "c".to_string(), startup: fsys::StartupMode::Eager }],
        );
        await!(children.lock()).insert(
            "c".to_string(),
            vec![ChildInfo { name: "d".to_string(), startup: fsys::StartupMode::Lazy }],
        );
        let uses = Arc::new(Mutex::new(HashMap::new()));
        resolver.register(
            "test".to_string(),
            Box::new(MockResolver { children: children.clone(), uses: uses.clone() }),
        );
        let model = Model::new(ModelParams {
            root_component_uri: "test:///root".to_string(),
            root_resolver_registry: resolver,
            root_default_runner: Box::new(runner),
        });

        // Bind to the top component, and check that it and the eager components were started.
        {
            let res = await!(model
                .bind_instance(AbsoluteMoniker::new(vec![ChildMoniker::new("a".to_string()),])));
            let expected_res: Result<(), ModelError> = Ok(());
            assert_eq!(format!("{:?}", res), format!("{:?}", expected_res));
            let actual_uris = await!(uris_run.lock());
            let expected_uris: Vec<String> = vec![
                "test:///a_resolved".to_string(),
                "test:///b_resolved".to_string(),
                "test:///c_resolved".to_string(),
            ];
            assert_eq!(*actual_uris, expected_uris);
        }
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn bind_instance_recursive_child() {
        let mut resolver = ResolverRegistry::new();
        let uris_run = Arc::new(Mutex::new(vec![]));
        let namespaces = Arc::new(Mutex::new(HashMap::new()));
        let outgoing_dirs = Arc::new(Mutex::new(HashMap::new()));
        let runner = MockRunner {
            uris_run: uris_run.clone(),
            namespaces: namespaces.clone(),
            outgoing_dirs: outgoing_dirs.clone(),
        };
        let children = Arc::new(Mutex::new(HashMap::new()));
        await!(children.lock()).insert(
            "root".to_string(),
            vec![ChildInfo { name: "system".to_string(), startup: fsys::StartupMode::Lazy }],
        );
        await!(children.lock()).insert(
            "system".to_string(),
            vec![
                ChildInfo { name: "logger".to_string(), startup: fsys::StartupMode::Lazy },
                ChildInfo { name: "netstack".to_string(), startup: fsys::StartupMode::Lazy },
            ],
        );
        let uses = Arc::new(Mutex::new(HashMap::new()));
        resolver.register(
            "test".to_string(),
            Box::new(MockResolver { children: children.clone(), uses: uses.clone() }),
        );
        let model = Model::new(ModelParams {
            root_component_uri: "test:///root".to_string(),
            root_resolver_registry: resolver,
            root_default_runner: Box::new(runner),
        });
        // bind to logger (before ever binding to system)
        {
            let res = await!(model.bind_instance(AbsoluteMoniker::new(vec![
                ChildMoniker::new("system".to_string()),
                ChildMoniker::new("logger".to_string())
            ])));
            let expected_res: Result<(), ModelError> = Ok(());
            assert_eq!(format!("{:?}", res), format!("{:?}", expected_res));
            let actual_uris = await!(uris_run.lock());
            let expected_uris: Vec<String> = vec!["test:///logger_resolved".to_string()];
            assert_eq!(*actual_uris, expected_uris);
        }
        // bind to netstack
        {
            let res = await!(model.bind_instance(AbsoluteMoniker::new(vec![
                ChildMoniker::new("system".to_string()),
                ChildMoniker::new("netstack".to_string()),
            ])));
            let expected_res: Result<(), ModelError> = Ok(());
            assert_eq!(format!("{:?}", res), format!("{:?}", expected_res));
            let actual_uris = await!(uris_run.lock());
            let expected_uris: Vec<String> = vec![
                "test:///logger_resolved".to_string(),
                "test:///netstack_resolved".to_string(),
            ];
            assert_eq!(*actual_uris, expected_uris);
        }
        // finally, bind to system
        {
            let res = await!(model.bind_instance(AbsoluteMoniker::new(vec![ChildMoniker::new(
                "system".to_string()
            ),])));
            let expected_res: Result<(), ModelError> = Ok(());
            assert_eq!(format!("{:?}", res), format!("{:?}", expected_res));
            let actual_uris = await!(uris_run.lock());
            let expected_uris: Vec<String> = vec![
                "test:///logger_resolved".to_string(),
                "test:///netstack_resolved".to_string(),
                "test:///system_resolved".to_string(),
            ];
            assert_eq!(*actual_uris, expected_uris);
        }
        // validate children
        {
            let actual_children = get_children(&*await!(model.root_realm.lock()));
            let mut expected_children: HashSet<ChildMoniker> = HashSet::new();
            expected_children.insert(ChildMoniker::new("system".to_string()));
            assert_eq!(actual_children, expected_children);
        }
        let system_realm = get_child_realm(&*await!(model.root_realm.lock()), "system");
        {
            let actual_children = get_children(&*await!(system_realm.lock()));
            let mut expected_children: HashSet<ChildMoniker> = HashSet::new();
            expected_children.insert(ChildMoniker::new("logger".to_string()));
            expected_children.insert(ChildMoniker::new("netstack".to_string()));
            assert_eq!(actual_children, expected_children);
        }
        {
            let logger_realm = get_child_realm(&*await!(system_realm.lock()), "logger");
            let actual_children = get_children(&*await!(logger_realm.lock()));
            assert!(actual_children.is_empty());
        }
        {
            let netstack_realm = get_child_realm(&*await!(system_realm.lock()), "netstack");
            let actual_children = get_children(&*await!(netstack_realm.lock()));
            assert!(actual_children.is_empty());
        }
    }
    #[fuchsia_async::run_singlethreaded(test)]
    async fn check_namespace_from_using() {
        let mut resolver = ResolverRegistry::new();
        let uris_run = Arc::new(Mutex::new(vec![]));
        let namespaces = Arc::new(Mutex::new(HashMap::new()));
        let outgoing_dirs = Arc::new(Mutex::new(HashMap::new()));
        let runner = MockRunner {
            uris_run: uris_run.clone(),
            namespaces: namespaces.clone(),
            outgoing_dirs: outgoing_dirs.clone(),
        };
        let children = Arc::new(Mutex::new(HashMap::new()));
        await!(children.lock()).insert(
            "root".to_string(),
            vec![ChildInfo { name: "system".to_string(), startup: fsys::StartupMode::Lazy }],
        );
        let uses = Arc::new(Mutex::new(HashMap::new()));

        // The system component will request namespaces of "/pkg" to its "/root_pkg" and "/foo/bar"
        await!(uses.lock()).insert(
            "system".to_string(),
            Some(vec![
                UseDecl {
                    type_: Some(CapabilityType::Directory),
                    source_path: Some("/pkg".to_string()),
                    target_path: Some("/root_pkg".to_string()),
                },
                UseDecl {
                    type_: Some(CapabilityType::Directory),
                    source_path: Some("/pkg".to_string()),
                    target_path: Some("/foo/bar".to_string()),
                },
            ]),
        );
        resolver.register(
            "test".to_string(),
            Box::new(MockResolver { children: children.clone(), uses: uses.clone() }),
        );
        let model = Model::new(ModelParams {
            root_component_uri: "test:///root".to_string(),
            root_resolver_registry: resolver,
            root_default_runner: Box::new(runner),
        });
        // bind to system
        let res = await!(model
            .bind_instance(AbsoluteMoniker::new(vec![ChildMoniker::new("system".to_string()),])));
        let expected_res: Result<(), ModelError> = Ok(());
        assert_eq!(format!("{:?}", res), format!("{:?}", expected_res));
        // Verify system has the expected namespaces.
        let mut actual_namespaces = await!(namespaces.lock());
        assert!(actual_namespaces.contains_key("test:///system_resolved"));
        let ns = actual_namespaces.get_mut("test:///system_resolved").unwrap();
        // "/pkg" is missing because we don't supply package in MockResolver.
        assert_eq!(ns.paths, ["/root_pkg", "/foo/bar"]);
        assert_eq!(ns.directories.len(), 2);
        // /root_pkg and /foo/bar are both pointed to the test component's /pkg.
        // Verify that we can read the dummy component manifest
        for _i in 0..ns.directories.len() {
            let dir = ns.directories.pop().unwrap();
            let dir_proxy = dir.into_proxy().unwrap();
            let path = PathBuf::from("meta/component_manager_tests_hello_world.cm");
            let file_proxy = io_util::open_file(&dir_proxy, &path).expect("could not open cm");
            await!(io_util::read_file(&file_proxy)).expect("could not read cm");
        }
    }
    fn get_children(realm: &Realm) -> HashSet<ChildMoniker> {
        realm.instance.child_realms.as_ref().unwrap().keys().map(|m| m.clone()).collect()
    }
    fn get_child_realm(realm: &Realm, child: &str) -> Arc<Mutex<Realm>> {
        realm.instance.child_realms.as_ref().unwrap()[&ChildMoniker::new(child.to_string())].clone()
    }
}
