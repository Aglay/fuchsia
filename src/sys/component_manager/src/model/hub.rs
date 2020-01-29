// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        capability::{
            CapabilityProvider, CapabilitySource, ComponentCapability, FrameworkCapability,
        },
        model::{
            addable_directory::AddableDirectoryWithResult,
            binding::ComponentDescriptor,
            dir_tree::{CapabilityUsageTree, DirTree},
            error::ModelError,
            hooks::{Event, EventPayload, EventType, Hook, HooksRegistration, RuntimeInfo},
            model::Model,
            moniker::AbsoluteMoniker,
            routing_facade::RoutingFacade,
        },
        path::PathBufExt,
    },
    async_trait::async_trait,
    cm_rust::ComponentDecl,
    directory_broker,
    fidl::endpoints::ServerEnd,
    fidl_fuchsia_io::{DirectoryProxy, NodeMarker, CLONE_FLAG_SAME_RIGHTS, MODE_TYPE_DIRECTORY},
    fuchsia_async::EHandle,
    fuchsia_trace as trace,
    fuchsia_vfs_pseudo_fs_mt::{
        directory::entry::DirectoryEntry, directory::immutable::simple as pfs,
        execution_scope::ExecutionScope, file::pcb::asynchronous::read_only_static,
        path::Path as pfsPath,
    },
    fuchsia_zircon as zx,
    futures::{future::BoxFuture, lock::Mutex},
    std::{
        collections::HashMap,
        path::PathBuf,
        sync::{Arc, Weak},
    },
};

// Declare simple directory type for brevity
type Directory = Arc<pfs::Simple>;

struct HubCapabilityProvider {
    abs_moniker: AbsoluteMoniker,
    relative_path: Vec<String>,
    hub_inner: Arc<HubInner>,
}

impl HubCapabilityProvider {
    pub fn new(
        abs_moniker: AbsoluteMoniker,
        relative_path: Vec<String>,
        hub_inner: Arc<HubInner>,
    ) -> Self {
        HubCapabilityProvider { abs_moniker, relative_path, hub_inner }
    }
}

#[async_trait]
impl CapabilityProvider for HubCapabilityProvider {
    async fn open(
        self: Box<Self>,
        flags: u32,
        open_mode: u32,
        in_relative_path: PathBuf,
        server_end: zx::Channel,
    ) -> Result<(), ModelError> {
        // Append relative_path to the back of the local relative_path vector, then convert it to a
        // pfsPath.
        let base_path: PathBuf = self.relative_path.iter().collect();
        let mut relative_path = base_path
            .attach(in_relative_path.clone())
            .to_str()
            .ok_or_else(|| ModelError::path_is_not_utf8(in_relative_path))?
            .to_string();
        relative_path.push('/');
        let dir_path = pfsPath::validate_and_split(relative_path.clone()).map_err(|_| {
            ModelError::open_directory_error(self.abs_moniker.clone(), relative_path)
        })?;
        self.hub_inner.open(&self.abs_moniker, flags, open_mode, dir_path, server_end).await?;

        Ok(())
    }
}

/// Hub state on an instance of a component.
struct Instance {
    pub abs_moniker: AbsoluteMoniker,
    pub component_url: String,
    pub execution: Option<Execution>,
    pub directory: Directory,
    pub children_directory: Directory,
    pub deleting_directory: Directory,
}

/// The execution state for a component that has started running.
struct Execution {
    pub resolved_url: String,
    pub capability_usage_tree: CapabilityUsageTree,
    pub directory: Directory,
}

/// The Hub is a directory tree representing the component topology. Through the Hub,
/// debugging and instrumentation tools can query information about component instances
/// on the system, such as their component URLs, execution state and so on.
///
/// Hub itself does not store any state locally other than a reference to `HubInner`
/// where all the state and business logic resides. This enables Hub to be cloneable
/// to be passed across tasks.
pub struct Hub {
    inner: Arc<HubInner>,
}

impl Hub {
    /// Create a new Hub given a `component_url` and a controller to the root directory.
    pub fn new(model: Weak<Model>, component_url: String) -> Result<Self, ModelError> {
        Ok(Hub { inner: Arc::new(HubInner::new(model, component_url)?) })
    }

    pub async fn open_root(&self, flags: u32, server_end: zx::Channel) -> Result<(), ModelError> {
        let root_moniker = AbsoluteMoniker::root();
        self.inner
            .open(&root_moniker, flags, MODE_TYPE_DIRECTORY, pfsPath::empty(), server_end)
            .await?;
        Ok(())
    }

    pub fn hooks(&self) -> Vec<HooksRegistration> {
        vec![HooksRegistration::new(
            "Hub",
            vec![
                EventType::AddDynamicChild,
                EventType::PostDestroyInstance,
                EventType::PreDestroyInstance,
                EventType::RouteCapability,
                EventType::BeforeStartInstance,
                EventType::StopInstance,
            ],
            Arc::downgrade(&self.inner) as Weak<dyn Hook>,
        )]
    }
}

struct HubInner {
    model: Weak<Model>,
    instances: Mutex<HashMap<AbsoluteMoniker, Instance>>,
    scope: ExecutionScope,
}

impl HubInner {
    /// Create a new Hub given a |component_url| and a controller to the root directory.
    pub fn new(model: Weak<Model>, component_url: String) -> Result<Self, ModelError> {
        let mut instances_map = HashMap::new();
        let abs_moniker = AbsoluteMoniker::root();

        HubInner::add_instance_if_necessary(&abs_moniker, component_url, &mut instances_map)?
            .expect("Did not create directory.");

        Ok(HubInner {
            model,
            instances: Mutex::new(instances_map),
            scope: ExecutionScope::from_executor(Box::new(EHandle::local())),
        })
    }

    pub async fn open(
        &self,
        abs_moniker: &AbsoluteMoniker,
        flags: u32,
        open_mode: u32,
        relative_path: pfsPath,
        server_end: zx::Channel,
    ) -> Result<(), ModelError> {
        let instances_map = self.instances.lock().await;
        if !instances_map.contains_key(&abs_moniker) {
            return Err(ModelError::open_directory_error(
                abs_moniker.clone(),
                relative_path.into_string(),
            ));
        }
        instances_map[&abs_moniker].directory.clone().open(
            self.scope.clone(),
            flags,
            open_mode,
            relative_path,
            ServerEnd::<NodeMarker>::new(server_end),
        );
        Ok(())
    }

    fn add_instance_if_necessary(
        abs_moniker: &AbsoluteMoniker,
        component_url: String,
        instance_map: &mut HashMap<AbsoluteMoniker, Instance>,
    ) -> Result<Option<Directory>, ModelError> {
        trace::duration!("component_manager", "hub:add_instance_if_necessary");
        if instance_map.contains_key(&abs_moniker) {
            return Ok(None);
        }

        let instance = pfs::simple();

        // Add a 'url' file.
        instance.add_node(
            "url",
            { read_only_static(component_url.clone().into_bytes()) },
            &abs_moniker,
        )?;

        // Add an 'id' file.
        // For consistency sake, the Hub assumes that the root instance also
        // has ID 0, like any other static instance.
        let id =
            if let Some(child_moniker) = abs_moniker.leaf() { child_moniker.instance() } else { 0 };
        let component_type = if id > 0 { "dynamic" } else { "static" };
        instance.add_node("id", { read_only_static(id.to_string().into_bytes()) }, &abs_moniker)?;

        // Add a 'component_type' file.
        instance.add_node(
            "component_type",
            { read_only_static(component_type.to_string().into_bytes()) },
            &abs_moniker,
        )?;

        // Add a children directory.
        let children = pfs::simple();
        instance.add_node("children", children.clone(), &abs_moniker)?;

        // Add a deleting directory.
        let deleting = pfs::simple();
        instance.add_node("deleting", deleting.clone(), &abs_moniker)?;

        instance_map.insert(
            abs_moniker.clone(),
            Instance {
                abs_moniker: abs_moniker.clone(),
                component_url,
                execution: None,
                directory: instance.clone(),
                children_directory: children.clone(),
                deleting_directory: deleting.clone(),
            },
        );

        Ok(Some(instance))
    }

    async fn add_instance_to_parent_if_necessary<'a>(
        abs_moniker: &'a AbsoluteMoniker,
        component_url: String,
        mut instances_map: &'a mut HashMap<AbsoluteMoniker, Instance>,
    ) -> Result<(), ModelError> {
        if let Some(controlled) =
            HubInner::add_instance_if_necessary(&abs_moniker, component_url, &mut instances_map)?
        {
            if let (Some(leaf), Some(parent_moniker)) = (abs_moniker.leaf(), abs_moniker.parent()) {
                // In the children directory, the child's instance id is not used
                trace::duration!("component_manager", "hub:add_instance_to_parent");
                let partial_moniker = leaf.to_partial();
                instances_map[&parent_moniker].children_directory.add_node(
                    partial_moniker.as_str(),
                    controlled.clone(),
                    &abs_moniker,
                )?;
            }
        }
        Ok(())
    }

    fn add_resolved_url_file(
        execution_directory: Directory,
        resolved_url: String,
        abs_moniker: &AbsoluteMoniker,
    ) -> Result<(), ModelError> {
        execution_directory.add_node(
            "resolved_url",
            { read_only_static(resolved_url.into_bytes()) },
            &abs_moniker,
        )?;
        Ok(())
    }

    fn add_in_directory(
        execution_directory: Directory,
        component_decl: ComponentDecl,
        package_dir: Option<DirectoryProxy>,
        routing_facade: &RoutingFacade,
        abs_moniker: &AbsoluteMoniker,
    ) -> Result<(), ModelError> {
        let tree = DirTree::build_from_uses(
            routing_facade.route_use_fn_factory(),
            abs_moniker,
            component_decl,
        );
        let mut in_dir = pfs::simple();
        tree.install(abs_moniker, &mut in_dir)?;
        if let Some(pkg_dir) = package_dir {
            in_dir.add_node(
                "pkg",
                directory_broker::DirectoryBroker::from_directory_proxy(pkg_dir),
                abs_moniker,
            )?;
        }
        execution_directory.add_node("in", in_dir, abs_moniker)?;
        Ok(())
    }

    fn add_expose_directory(
        execution_directory: Directory,
        component_decl: ComponentDecl,
        routing_facade: &RoutingFacade,
        abs_moniker: &AbsoluteMoniker,
    ) -> Result<(), ModelError> {
        trace::duration!("component_manager", "hub:add_expose_directory");
        let tree = DirTree::build_from_exposes(
            routing_facade.route_expose_fn_factory(),
            abs_moniker,
            component_decl,
        );
        let mut expose_dir = pfs::simple();
        tree.install(abs_moniker, &mut expose_dir)?;
        execution_directory.add_node("expose", expose_dir, abs_moniker)?;
        Ok(())
    }

    fn add_out_directory(
        execution_directory: Directory,
        outgoing_dir: Option<DirectoryProxy>,
        abs_moniker: &AbsoluteMoniker,
    ) -> Result<(), ModelError> {
        trace::duration!("component_manager", "hub:add_out_directory");
        if let Some(out_dir) = outgoing_dir {
            execution_directory.add_node(
                "out",
                directory_broker::DirectoryBroker::from_directory_proxy(out_dir),
                abs_moniker,
            )?;
        }
        Ok(())
    }

    fn add_runtime_directory(
        execution_directory: Directory,
        runtime_dir: Option<DirectoryProxy>,
        abs_moniker: &AbsoluteMoniker,
    ) -> Result<(), ModelError> {
        trace::duration!("component_manager", "hub:add_runtime_directory");
        if let Some(runtime_dir) = runtime_dir {
            execution_directory.add_node(
                "runtime",
                directory_broker::DirectoryBroker::from_directory_proxy(runtime_dir),
                abs_moniker,
            )?;
        }
        Ok(())
    }

    async fn on_before_start_instance_async<'a>(
        &'a self,
        target_moniker: &AbsoluteMoniker,
        runtime: &RuntimeInfo,
        component_decl: &'a ComponentDecl,
        live_children: &'a Vec<ComponentDescriptor>,
        routing_facade: RoutingFacade,
    ) -> Result<(), ModelError> {
        trace::duration!("component_manager", "hub:on_start_instance_async");
        let model = self.model.upgrade().ok_or(ModelError::ModelNotAvailable)?;
        let realm = model.look_up_realm(target_moniker).await?;
        let component_url = realm.component_url.clone();

        let mut instances_map = self.instances.lock().await;

        Self::add_instance_to_parent_if_necessary(
            target_moniker,
            component_url,
            &mut instances_map,
        )
        .await?;

        let instance = instances_map
            .get_mut(target_moniker)
            .expect(&format!("Unable to find instance {} in map.", target_moniker));

        // If we haven't already created an execution directory, create one now.
        if instance.execution.is_none() {
            trace::duration!("component_manager", "hub:create_execution");
            let execution_directory = pfs::simple();

            let used = pfs::simple();

            let capability_usage_tree =
                CapabilityUsageTree::new(used.clone(), routing_facade.clone());
            let exec = Execution {
                resolved_url: runtime.resolved_url.clone(),
                directory: execution_directory.clone(),
                capability_usage_tree,
            };
            instance.execution = Some(exec);

            Self::add_resolved_url_file(
                execution_directory.clone(),
                runtime.resolved_url.clone(),
                target_moniker,
            )?;

            Self::add_in_directory(
                execution_directory.clone(),
                component_decl.clone(),
                Self::clone_dir(runtime.package_dir.as_ref()),
                &routing_facade,
                target_moniker,
            )?;

            Self::add_expose_directory(
                execution_directory.clone(),
                component_decl.clone(),
                &routing_facade,
                &target_moniker,
            )?;

            execution_directory.add_node("used", used, &target_moniker)?;
            Self::add_out_directory(
                execution_directory.clone(),
                Self::clone_dir(runtime.outgoing_dir.as_ref()),
                &target_moniker,
            )?;

            Self::add_runtime_directory(
                execution_directory.clone(),
                Self::clone_dir(runtime.runtime_dir.as_ref()),
                &target_moniker,
            )?;

            instance.directory.add_node("exec", execution_directory, &target_moniker)?;
        }

        // TODO: Loop over deleting realms also?
        for child_descriptor in live_children {
            let abs_moniker = child_descriptor.abs_moniker.to_string();
            trace::duration!("component_manager", "hub:add_live_child",
                             "child_moniker" => abs_moniker.as_ref());
            Self::add_instance_to_parent_if_necessary(
                &child_descriptor.abs_moniker,
                child_descriptor.url.clone(),
                &mut instances_map,
            )
            .await?;
        }

        Ok(())
    }

    async fn on_add_dynamic_child_async(
        &self,
        target_moniker: &AbsoluteMoniker,
        component_url: String,
    ) -> Result<(), ModelError> {
        trace::duration!("component_manager", "hub:on_add_dynamic_child_async");
        let mut instances_map = self.instances.lock().await;
        Self::add_instance_to_parent_if_necessary(
            target_moniker,
            component_url,
            &mut instances_map,
        )
        .await?;
        Ok(())
    }

    async fn on_post_destroy_instance_async(
        &self,
        target_moniker: &AbsoluteMoniker,
    ) -> Result<(), ModelError> {
        trace::duration!("component_manager", "hub:on_post_destroy_instance_async");
        let mut instance_map = self.instances.lock().await;

        // TODO(xbhatnag): Investigate error handling scenarios here.
        //                 Can these errors arise from faulty components or from
        //                 a bug in ComponentManager?
        let parent_moniker = target_moniker.parent().expect("a root component cannot be dynamic");
        let leaf = target_moniker.leaf().expect("a root component cannot be dynamic");

        instance_map[&parent_moniker].deleting_directory.remove_node(leaf.as_str())?;
        instance_map
            .remove(&target_moniker)
            .expect("the dynamic component must exist in the instance map");
        Ok(())
    }

    async fn on_stop_instance_async(
        &self,
        target_moniker: &AbsoluteMoniker,
    ) -> Result<(), ModelError> {
        trace::duration!("component_manager", "hub:on_stop_instance_async");
        let mut instance_map = self.instances.lock().await;
        instance_map[target_moniker].directory.remove_node("exec")?;
        instance_map.get_mut(target_moniker).expect("instance must exist").execution = None;
        Ok(())
    }

    async fn on_pre_destroy_instance_async(
        &self,
        target_moniker: &AbsoluteMoniker,
    ) -> Result<(), ModelError> {
        trace::duration!("component_manager", "hub:on_pre_destroy_instance_async");
        let parent_moniker = target_moniker.parent().expect("A root component cannot be destroyed");
        let instance_map = self.instances.lock().await;
        if !instance_map.contains_key(&parent_moniker) {
            // Evidently this a duplicate dispatch of PreDestroyInstance.
            return Ok(());
        }

        let leaf = target_moniker.leaf().expect("A root component cannot be destroyed");

        // In the children directory, the child's instance id is not used
        // TODO: It's possible for the PreDestroyInstance event to be dispatched twice if there
        // are two concurrent `DestroyChild` operations. In such cases we should probably cause
        // this update to no-op instead of returning an error.
        let partial_moniker = leaf.to_partial();
        let directory = instance_map[&parent_moniker]
            .children_directory
            .remove_node(partial_moniker.as_str())
            .map_err(|_| ModelError::remove_entry_error(leaf.as_str()))?;

        instance_map[&parent_moniker].deleting_directory.add_node(
            leaf.as_str(),
            directory,
            target_moniker,
        )?;
        Ok(())
    }

    /// Given a `CapabilitySource`, determine if it is a framework-provided
    /// hub capability. If so, update the given `capability_provider` to
    /// provide a hub directory.
    async fn try_provide_hub_capability<'a>(
        self: Arc<Self>,
        source: &'a CapabilitySource,
        capability_provider: Arc<Mutex<Option<Box<dyn CapabilityProvider>>>>,
    ) {
        trace::duration!("component_manager", "hub:try_provide_hub_capability");
        // If this is a scoped framework directory capability, then check the source path
        if let CapabilitySource::Framework {
            capability: FrameworkCapability::Directory(source_path),
            scope_moniker: Some(scope_moniker),
        } = source
        {
            let mut relative_path = source_path.split();
            // The source path must begin with "hub"
            if relative_path.is_empty() || relative_path.remove(0) != "hub" {
                return;
            }

            // Set the capability provider, if not already set.
            let mut capability_provider = capability_provider.lock().await;
            if capability_provider.is_none() {
                *capability_provider = Some(Box::new(HubCapabilityProvider::new(
                    scope_moniker.clone(),
                    relative_path,
                    self,
                )))
            }
        }
    }

    async fn on_route_capability_async(
        self: Arc<Self>,
        target_moniker: &AbsoluteMoniker,
        source: CapabilitySource,
        capability_provider: Arc<Mutex<Option<Box<dyn CapabilityProvider>>>>,
    ) -> Result<(), ModelError> {
        trace::duration!("component_manager", "hub:on_route_capability_async");
        self.clone().try_provide_hub_capability(&source, capability_provider).await;

        // Track used capabilities.
        if Self::is_capability_visible_in_namespace(&source) {
            let mut instance_map = self.instances.lock().await;
            let execution = instance_map
                .get_mut(&target_moniker)
                .expect(
                    "A component that is requesting a capability must exist in the instance map",
                )
                .execution
                .as_mut();
            if execution.is_none() {
                // We won't normally get here, but it's possible if a runner continued to execute a
                // "zombie" component, and a capability was used, after component manager marked the
                // component stopped.
                return Ok(());
            }
            execution
                .unwrap()
                .capability_usage_tree
                .mark_capability_used(target_moniker, source)
                .await?;
        }
        Ok(())
    }

    // TODO(fsamuel): We should probably preserve the original error messages
    // instead of dropping them.
    fn clone_dir(dir: Option<&DirectoryProxy>) -> Option<DirectoryProxy> {
        dir.and_then(|d| io_util::clone_directory(d, CLONE_FLAG_SAME_RIGHTS).ok())
    }

    /// Return if the given capability is exposed in the component's namespace.
    ///
    /// Services may be exposed in a component's namespace. Runner caps, storage
    /// caps, resolvers etc are not exposed in a components namespace.
    fn is_capability_visible_in_namespace(source: &CapabilitySource) -> bool {
        match source {
            CapabilitySource::Framework { capability: FrameworkCapability::Runner(_), .. } => false,
            CapabilitySource::Component { capability: ComponentCapability::Runner(_), .. } => false,
            CapabilitySource::StorageDecl(_, _) => false,
            _ => true,
        }
    }
}

impl Hook for HubInner {
    fn on(self: Arc<Self>, event: &Event) -> BoxFuture<Result<(), ModelError>> {
        Box::pin(async move {
            match &event.payload {
                EventPayload::BeforeStartInstance {
                    runtime,
                    component_decl,
                    live_children,
                    routing_facade,
                } => {
                    self.on_before_start_instance_async(
                        &event.target_moniker,
                        &runtime,
                        &component_decl,
                        &live_children,
                        routing_facade.clone(),
                    )
                    .await?;
                }
                EventPayload::AddDynamicChild { component_url } => {
                    self.on_add_dynamic_child_async(
                        &event.target_moniker,
                        component_url.to_string(),
                    )
                    .await?;
                }
                EventPayload::PreDestroyInstance => {
                    self.on_pre_destroy_instance_async(&event.target_moniker).await?;
                }
                EventPayload::StopInstance => {
                    self.on_stop_instance_async(&event.target_moniker).await?;
                }
                EventPayload::PostDestroyInstance => {
                    self.on_post_destroy_instance_async(&event.target_moniker).await?;
                }
                EventPayload::RouteCapability { source, capability_provider } => {
                    self.on_route_capability_async(
                        &event.target_moniker,
                        source.clone(),
                        capability_provider.clone(),
                    )
                    .await?;
                }
                _ => {}
            };
            Ok(())
        })
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        crate::{
            builtin_environment::BuiltinEnvironment,
            model::{
                binding::Binder,
                model::{ComponentManagerConfig, Model, ModelParams},
                resolver::ResolverRegistry,
                rights,
                testing::mocks,
                testing::{
                    test_helpers::*,
                    test_helpers::{
                        dir_contains, list_directory, list_directory_recursive, read_file,
                    },
                    test_hook::HubInjectionTestHook,
                },
            },
            startup,
        },
        cm_rust::{
            self, CapabilityPath, ComponentDecl, ExposeDecl, ExposeDirectoryDecl,
            ExposeProtocolDecl, ExposeSource, ExposeTarget, UseDecl, UseDirectoryDecl,
            UseProtocolDecl, UseSource,
        },
        fidl::endpoints::ServerEnd,
        fidl_fuchsia_io::{
            DirectoryMarker, DirectoryProxy, MODE_TYPE_DIRECTORY, OPEN_RIGHT_READABLE,
            OPEN_RIGHT_WRITABLE,
        },
        fidl_fuchsia_io2 as fio2,
        fuchsia_async::EHandle,
        fuchsia_vfs_pseudo_fs_mt::{
            directory::entry::DirectoryEntry, execution_scope::ExecutionScope,
            file::pcb::asynchronous::read_only_static, path::Path as pfsPath, pseudo_directory,
        },
        std::{convert::TryFrom, path::Path},
    };

    /// Hosts an out directory with a 'foo' file.
    fn foo_out_dir_fn() -> Box<dyn Fn(ServerEnd<DirectoryMarker>) + Send + Sync> {
        Box::new(move |server_end: ServerEnd<DirectoryMarker>| {
            let out_dir = pseudo_directory!(
                "foo" => read_only_static(b"bar"),
                "test" => pseudo_directory!(
                    "aaa" => read_only_static(b"bbb"),
                ),
            );

            out_dir.clone().open(
                ExecutionScope::from_executor(Box::new(EHandle::local())),
                OPEN_RIGHT_READABLE | OPEN_RIGHT_WRITABLE,
                MODE_TYPE_DIRECTORY,
                pfsPath::empty(),
                ServerEnd::new(server_end.into_channel()),
            );
        })
    }

    /// Hosts a runtime directory with a 'bleep' file.
    fn bleep_runtime_dir_fn() -> Box<dyn Fn(ServerEnd<DirectoryMarker>) + Send + Sync> {
        Box::new(move |server_end: ServerEnd<DirectoryMarker>| {
            let pseudo_dir = pseudo_directory!(
                "bleep" => read_only_static(b"blah"),
            );

            pseudo_dir.clone().open(
                ExecutionScope::from_executor(Box::new(EHandle::local())),
                OPEN_RIGHT_READABLE | OPEN_RIGHT_WRITABLE,
                MODE_TYPE_DIRECTORY,
                pfsPath::empty(),
                ServerEnd::new(server_end.into_channel()),
            );
        })
    }

    type DirectoryCallback = Box<dyn Fn(ServerEnd<DirectoryMarker>) + Send + Sync>;

    struct ComponentDescriptor {
        pub name: String,
        pub decl: ComponentDecl,
        pub host_fn: Option<DirectoryCallback>,
        pub runtime_host_fn: Option<DirectoryCallback>,
    }

    async fn start_component_manager_with_hub(
        root_component_url: String,
        components: Vec<ComponentDescriptor>,
    ) -> (Arc<Model>, BuiltinEnvironment, DirectoryProxy) {
        start_component_manager_with_hub_and_hooks(root_component_url, components, vec![]).await
    }

    async fn start_component_manager_with_hub_and_hooks(
        root_component_url: String,
        components: Vec<ComponentDescriptor>,
        additional_hooks: Vec<HooksRegistration>,
    ) -> (Arc<Model>, BuiltinEnvironment, DirectoryProxy) {
        let resolved_root_component_url = format!("{}_resolved", root_component_url);
        let mut resolver = ResolverRegistry::new();
        let runner = Arc::new(mocks::MockRunner::new());
        let mut mock_resolver = mocks::MockResolver::new();
        for component in components.into_iter() {
            mock_resolver.add_component(&component.name, component.decl);
            if let Some(host_fn) = component.host_fn {
                runner.add_host_fn(&resolved_root_component_url, host_fn);
            }

            if let Some(runtime_host_fn) = component.runtime_host_fn {
                runner.add_runtime_host_fn(&resolved_root_component_url, runtime_host_fn);
            }
        }
        resolver.register("test".to_string(), Box::new(mock_resolver));

        let startup_args = startup::Arguments {
            use_builtin_process_launcher: false,
            use_builtin_vmex: false,
            root_component_url: root_component_url.clone(),
            debug: false,
        };
        let model = Arc::new(Model::new(ModelParams {
            root_component_url,
            root_resolver_registry: resolver,
            elf_runner: runner.clone(),
            builtin_runners: vec![(TEST_RUNNER_NAME.into(), runner.clone() as _)]
                .into_iter()
                .collect(),
        }));
        let builtin_environment =
            BuiltinEnvironment::new(&startup_args, &model, ComponentManagerConfig::default())
                .await
                .expect("failed to set up builtin environment");
        let hub_proxy = builtin_environment
            .bind_service_fs_for_hub(&model)
            .await
            .expect("unable to bind service_fs");

        model.root_realm.hooks.install(additional_hooks).await;

        let root_moniker = AbsoluteMoniker::root();
        let res = model.bind(&root_moniker).await;
        assert!(res.is_ok());

        (model, builtin_environment, hub_proxy)
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn hub_basic() {
        let root_component_url = "test:///root".to_string();
        let (_model, _builtin_environment, hub_proxy) = start_component_manager_with_hub(
            root_component_url.clone(),
            vec![
                ComponentDescriptor {
                    name: "root".to_string(),
                    decl: ComponentDeclBuilder::new()
                        .add_lazy_child("a")
                        .offer_runner_to_children(TEST_RUNNER_NAME)
                        .build(),
                    host_fn: None,
                    runtime_host_fn: None,
                },
                ComponentDescriptor {
                    name: "a".to_string(),
                    decl: component_decl_with_test_runner(),
                    host_fn: None,
                    runtime_host_fn: None,
                },
            ],
        )
        .await;

        assert_eq!(root_component_url, read_file(&hub_proxy, "url").await);
        assert_eq!(
            format!("{}_resolved", root_component_url),
            read_file(&hub_proxy, "exec/resolved_url").await
        );

        // Verify IDs
        assert_eq!("0", read_file(&hub_proxy, "id").await);
        assert_eq!("0", read_file(&hub_proxy, "children/a/id").await);

        // Verify Component Type
        assert_eq!("static", read_file(&hub_proxy, "component_type").await);
        assert_eq!("static", read_file(&hub_proxy, "children/a/component_type").await);

        assert_eq!("test:///a", read_file(&hub_proxy, "children/a/url").await);
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn hub_out_directory() {
        let root_component_url = "test:///root".to_string();
        let (_model, _builtin_environment, hub_proxy) = start_component_manager_with_hub(
            root_component_url.clone(),
            vec![ComponentDescriptor {
                name: "root".to_string(),
                decl: ComponentDeclBuilder::new()
                    .add_lazy_child("a")
                    .offer_runner_to_children(TEST_RUNNER_NAME)
                    .build(),
                host_fn: Some(foo_out_dir_fn()),
                runtime_host_fn: None,
            }],
        )
        .await;

        assert!(dir_contains(&hub_proxy, "exec", "out").await);
        assert!(dir_contains(&hub_proxy, "exec/out", "foo").await);
        assert!(dir_contains(&hub_proxy, "exec/out/test", "aaa").await);
        assert_eq!("bar", read_file(&hub_proxy, "exec/out/foo").await);
        assert_eq!("bbb", read_file(&hub_proxy, "exec/out/test/aaa").await);
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn hub_runtime_directory() {
        let root_component_url = "test:///root".to_string();
        let (_model, _builtin_environment, hub_proxy) = start_component_manager_with_hub(
            root_component_url.clone(),
            vec![ComponentDescriptor {
                name: "root".to_string(),
                decl: ComponentDeclBuilder::new()
                    .add_lazy_child("a")
                    .offer_runner_to_children(TEST_RUNNER_NAME)
                    .build(),
                host_fn: None,
                runtime_host_fn: Some(bleep_runtime_dir_fn()),
            }],
        )
        .await;

        assert_eq!("blah", read_file(&hub_proxy, "exec/runtime/bleep").await);
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn hub_test_hook_interception() {
        let root_component_url = "test:///root".to_string();
        let hub_injection_test_hook = Arc::new(HubInjectionTestHook::new());
        let (_model, _builtin_environment, hub_proxy) = start_component_manager_with_hub_and_hooks(
            root_component_url.clone(),
            vec![ComponentDescriptor {
                name: "root".to_string(),
                decl: ComponentDeclBuilder::new()
                    .add_lazy_child("a")
                    .use_(UseDecl::Directory(UseDirectoryDecl {
                        source: UseSource::Framework,
                        source_path: CapabilityPath::try_from("/hub").unwrap(),
                        target_path: CapabilityPath::try_from("/hub").unwrap(),
                        rights: *rights::READ_RIGHTS | *rights::WRITE_RIGHTS,
                        subdir: None,
                    }))
                    .offer_runner_to_children(TEST_RUNNER_NAME)
                    .build(),
                host_fn: None,
                runtime_host_fn: None,
            }],
            vec![HooksRegistration::new(
                "HubInjectionTestHook",
                vec![EventType::RouteCapability],
                Arc::downgrade(&hub_injection_test_hook) as Weak<dyn Hook>,
            )],
        )
        .await;

        let in_dir = io_util::open_directory(
            &hub_proxy,
            &Path::new("exec/in"),
            OPEN_RIGHT_READABLE | OPEN_RIGHT_WRITABLE,
        )
        .expect("Failed to open directory");
        assert_eq!(vec!["hub"], list_directory(&in_dir).await);

        let scoped_hub_dir_proxy = io_util::open_directory(
            &hub_proxy,
            &Path::new("exec/in/hub"),
            OPEN_RIGHT_READABLE | OPEN_RIGHT_WRITABLE,
        )
        .expect("Failed to open directory");
        // There are no out or runtime directories because there is no program running.
        assert_eq!(vec!["old_hub"], list_directory(&scoped_hub_dir_proxy).await);

        let old_hub_dir_proxy = io_util::open_directory(
            &hub_proxy,
            &Path::new("exec/in/hub/old_hub/exec"),
            OPEN_RIGHT_READABLE | OPEN_RIGHT_WRITABLE,
        )
        .expect("Failed to open directory");
        assert_eq!(
            vec!["expose", "in", "out", "resolved_url", "runtime", "used"],
            list_directory(&old_hub_dir_proxy).await
        );
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn hub_ensure_runners_not_exposed_in_used() {
        let root_component_url = "test:///root".to_string();

        // Start a root component and child component, both using the runner "test_runner".
        let (_model, _builtin_environment, hub_proxy) = start_component_manager_with_hub(
            root_component_url.clone(),
            vec![ComponentDescriptor {
                name: "root".to_string(),
                decl: component_decl_with_test_runner(),
                host_fn: None,
                runtime_host_fn: None,
            }],
        )
        .await;

        // Ensure "used" directory can be opened, but is empty.
        let in_dir = io_util::open_directory(
            &hub_proxy,
            &Path::new("exec/used"),
            OPEN_RIGHT_READABLE | OPEN_RIGHT_WRITABLE,
        )
        .expect("Failed to open directory");
        assert_eq!(Vec::<String>::new(), list_directory(&in_dir).await);
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn hub_in_directory() {
        let root_component_url = "test:///root".to_string();
        let (_model, _builtin_environment, hub_proxy) = start_component_manager_with_hub(
            root_component_url.clone(),
            vec![ComponentDescriptor {
                name: "root".to_string(),
                decl: ComponentDeclBuilder::new()
                    .add_lazy_child("a")
                    .use_(UseDecl::Directory(UseDirectoryDecl {
                        source: UseSource::Framework,
                        source_path: CapabilityPath::try_from("/hub/exec").unwrap(),
                        target_path: CapabilityPath::try_from("/hub").unwrap(),
                        rights: *rights::READ_RIGHTS | *rights::WRITE_RIGHTS,
                        subdir: None,
                    }))
                    .use_(UseDecl::Protocol(UseProtocolDecl {
                        source: UseSource::Realm,
                        source_path: CapabilityPath::try_from("/svc/baz").unwrap(),
                        target_path: CapabilityPath::try_from("/svc/hippo").unwrap(),
                    }))
                    .use_(UseDecl::Directory(UseDirectoryDecl {
                        source: UseSource::Realm,
                        source_path: CapabilityPath::try_from("/data/foo").unwrap(),
                        target_path: CapabilityPath::try_from("/data/bar").unwrap(),
                        rights: *rights::READ_RIGHTS | *rights::WRITE_RIGHTS,
                        subdir: None,
                    }))
                    .offer_runner_to_children(TEST_RUNNER_NAME)
                    .build(),
                host_fn: None,
                runtime_host_fn: None,
            }],
        )
        .await;

        let in_dir = io_util::open_directory(
            &hub_proxy,
            &Path::new("exec/in"),
            OPEN_RIGHT_READABLE | OPEN_RIGHT_WRITABLE,
        )
        .expect("Failed to open directory");
        assert_eq!(vec!["data", "hub", "svc"], list_directory(&in_dir).await);

        let scoped_hub_dir_proxy = io_util::open_directory(
            &hub_proxy,
            &Path::new("exec/in/hub"),
            OPEN_RIGHT_READABLE | OPEN_RIGHT_WRITABLE,
        )
        .expect("Failed to open directory");
        assert_eq!(
            vec!["expose", "in", "out", "resolved_url", "runtime", "used"],
            list_directory(&scoped_hub_dir_proxy).await
        );
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn hub_expose_directory() {
        let root_component_url = "test:///root".to_string();
        let (_model, _builtin_environment, hub_proxy) = start_component_manager_with_hub(
            root_component_url.clone(),
            vec![ComponentDescriptor {
                name: "root".to_string(),
                decl: ComponentDeclBuilder::new()
                    .add_lazy_child("a")
                    .expose(ExposeDecl::Protocol(ExposeProtocolDecl {
                        source: ExposeSource::Self_,
                        source_path: CapabilityPath::try_from("/svc/foo").unwrap(),
                        target_path: CapabilityPath::try_from("/svc/bar").unwrap(),
                        target: ExposeTarget::Realm,
                    }))
                    .expose(ExposeDecl::Directory(ExposeDirectoryDecl {
                        source: ExposeSource::Self_,
                        source_path: CapabilityPath::try_from("/data/baz").unwrap(),
                        target_path: CapabilityPath::try_from("/data/hippo").unwrap(),
                        target: ExposeTarget::Realm,
                        rights: Some(fio2::Operations::Connect),
                        subdir: None,
                    }))
                    .offer_runner_to_children(TEST_RUNNER_NAME)
                    .build(),
                host_fn: None,
                runtime_host_fn: None,
            }],
        )
        .await;

        let expose_dir = io_util::open_directory(
            &hub_proxy,
            &Path::new("exec/expose"),
            OPEN_RIGHT_READABLE | OPEN_RIGHT_WRITABLE,
        )
        .expect("Failed to open directory");
        assert_eq!(vec!["data/hippo", "svc/bar"], list_directory_recursive(&expose_dir).await);
    }
}
