// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

pub mod error;
pub use error::RoutingError;

use {
    crate::{
        capability::{
            CapabilityProvider, CapabilitySource, ComponentCapability, EnvironmentCapability,
            InternalCapability,
        },
        channel,
        model::{
            component::{
                BindReason, ComponentInstance, ComponentManagerInstance, ExtendedInstance,
                WeakComponentInstance,
            },
            error::ModelError,
            events::{filter::EventFilter, mode_set::EventModeSet},
            hooks::{Event, EventPayload},
            logging::{FmtArgsLogger, LOGGER as MODEL_LOGGER},
            rights::{Rights, READ_RIGHTS, WRITE_RIGHTS},
            storage,
            walk_state::WalkState,
        },
        path::PathBufExt,
    },
    async_trait::async_trait,
    cm_rust::{
        self, CapabilityDecl, CapabilityName, CapabilityPath, ComponentDecl, ExposeDecl,
        ExposeDirectoryDecl, ExposeSource, ExposeTarget, OfferDecl, OfferDirectoryDecl,
        OfferDirectorySource, OfferEventDecl, OfferEventSource, OfferResolverSource,
        OfferRunnerSource, OfferServiceSource, OfferStorageSource, StorageDecl,
        StorageDirectorySource, UseDecl, UseDirectoryDecl, UseEventDecl, UseProtocolDecl,
        UseSource, UseStorageDecl,
    },
    fidl::{endpoints::ServerEnd, epitaph::ChannelEpitaphExt},
    fidl_fuchsia_io as fio, fuchsia_zircon as zx,
    futures::lock::Mutex,
    log::*,
    moniker::{AbsoluteMoniker, ChildMoniker, ExtendedMoniker, PartialMoniker, RelativeMoniker},
    std::{path::PathBuf, sync::Arc},
};

const SERVICE_OPEN_FLAGS: u32 =
    fio::OPEN_FLAG_DESCRIBE | fio::OPEN_RIGHT_READABLE | fio::OPEN_RIGHT_WRITABLE;

/// Describes the source of a capability, for any type of capability.
#[derive(Debug)]
enum OfferSource<'a> {
    // TODO(fxbug.dev/4776): Enable this once unified services are implemented.
    #[allow(dead_code)]
    Service(&'a OfferServiceSource),
    Protocol(&'a OfferServiceSource),
    Directory(&'a OfferDirectorySource),
    Storage(&'a OfferStorageSource),
    Runner(&'a OfferRunnerSource),
    Event(&'a OfferEventSource),
    Resolver(&'a OfferResolverSource),
}

/// Describes the source of a capability, for any type of capability.
#[derive(Debug)]
enum CapabilityExposeSource<'a> {
    Protocol(&'a ExposeSource),
    Directory(&'a ExposeSource),
    Runner(&'a ExposeSource),
    Resolver(&'a ExposeSource),
}

/// Finds the source of the `capability` used by `absolute_moniker`, and pass along the
/// `server_chan` to the hosting component's out directory (or componentmgr's namespace, if
/// applicable) using an open request with `open_mode`.
pub(super) async fn route_use_capability<'a>(
    flags: u32,
    open_mode: u32,
    relative_path: String,
    use_decl: &'a UseDecl,
    target: &'a Arc<ComponentInstance>,
    server_chan: &mut zx::Channel,
) -> Result<(), ModelError> {
    match use_decl {
        UseDecl::Service(_) | UseDecl::Protocol(_) | UseDecl::Directory(_) | UseDecl::Runner(_) => {
            let (source, cap_state) = find_used_capability_source(use_decl, target).await?;
            match use_decl {
                UseDecl::Protocol(UseProtocolDecl { source: UseSource::Debug, .. }) => {
                    // TODO(anmittal): add and validate security policy
                }
                _ => {
                    //do nothing
                }
            }
            let relative_path = cap_state.make_relative_path(relative_path);
            open_capability_at_source(flags, open_mode, relative_path, source, target, server_chan)
                .await
        }
        UseDecl::Storage(storage_decl) => {
            // TODO(fxbug.dev/50716): This BindReason is wrong. We need to refactor the Storage
            // capability to plumb through the correct BindReason.
            route_and_open_storage_capability(
                storage_decl,
                open_mode,
                target,
                server_chan,
                &BindReason::Eager,
            )
            .await
        }
        UseDecl::Event(_) | UseDecl::EventStream(_) => {
            // Events are logged separately through route_use_event_capability.
            Ok(())
        }
    }
}

pub(super) async fn route_use_event_capability<'a>(
    use_decl: &'a UseDecl,
    target: &'a Arc<ComponentInstance>,
) -> Result<CapabilitySource, ModelError> {
    let (source, _cap_state) = find_used_capability_source(use_decl, target).await?;
    target.try_get_context()?.policy().can_route_capability(&source, &target.abs_moniker)?;
    Ok(source)
}

/// Finds the source of a capability that is registered with an environment, and
/// opens the capability with the given server-side channel.
/// TODO(61304): Make runner capability routing use this method.
pub(super) async fn route_capability_from_environment<'a>(
    flags: u32,
    open_mode: u32,
    relative_path: String,
    capability: EnvironmentCapability,
    target: &'a Arc<ComponentInstance>,
    server_chan: &mut zx::Channel,
) -> Result<(), ModelError> {
    let source = capability.registration_source().clone();
    let capability = ComponentCapability::Environment(capability);
    let cap_state = CapabilityState::Other;
    let (cap_source, cap_state) =
        find_environment_component_capability_source(&target, capability, cap_state, &source)
            .await?;

    let relative_path = cap_state.make_relative_path(relative_path);
    open_capability_at_source(flags, open_mode, relative_path, cap_source, target, server_chan)
        .await
}

/// Finds the source of the expose capability used at `source_path` by `target`, and pass
/// along the `server_chan` to the hosting component's out directory (or componentmgr's namespace,
/// if applicable)
pub(super) async fn route_expose_capability<'a>(
    flags: u32,
    open_mode: u32,
    relative_path: String,
    expose_decl: &'a ExposeDecl,
    target: &'a Arc<ComponentInstance>,
    server_chan: &mut zx::Channel,
) -> Result<(), ModelError> {
    let capability = ComponentCapability::UsedExpose(expose_decl.clone());
    let cap_state = CapabilityState::new(&capability);
    let mut pos = WalkPosition {
        capability,
        cap_state,
        last_child_moniker: None,
        current: ExtendedInstance::Component(target.clone()),
    };
    let source = walk_expose_chain(&mut pos).await?;
    let relative_path = pos.cap_state.make_relative_path(relative_path);
    open_capability_at_source(flags, open_mode, relative_path, source, target, server_chan).await
}

/// The default provider for a ComponentCapability.
/// This provider will bind to the source moniker's component and then open the service
/// from the component's outgoing directory.
struct DefaultComponentCapabilityProvider {
    target: WeakComponentInstance,
    source: WeakComponentInstance,
    name: CapabilityName,
    path: CapabilityPath,
}

#[async_trait]
impl CapabilityProvider for DefaultComponentCapabilityProvider {
    async fn open(
        self: Box<Self>,
        flags: u32,
        open_mode: u32,
        relative_path: PathBuf,
        server_end: &mut zx::Channel,
    ) -> Result<(), ModelError> {
        let capability = Arc::new(Mutex::new(Some(channel::take_channel(server_end))));
        // Start the source component, if necessary
        let path = self.path.to_path_buf().attach(relative_path);
        let source = self
            .source
            .upgrade()?
            .bind(&BindReason::AccessCapability {
                target: ExtendedMoniker::ComponentInstance(self.target.moniker.clone()),
                path: self.path.clone(),
            })
            .await?;

        let event = Event::new(
            &self.target.upgrade()?,
            Ok(EventPayload::CapabilityRequested {
                source_moniker: source.abs_moniker.clone(),
                name: self.name.to_string(),
                capability: capability.clone(),
            }),
        );
        source.hooks.dispatch(&event).await?;

        // If the capability transported through the event above wasn't transferred
        // out, then we can open the capability through the component's outgoing directory.
        // If some hook consumes the capability, then we don't bother looking in the outgoing
        // directory.
        let capability = capability.lock().await.take();
        if let Some(mut server_end_for_event) = capability {
            if let Err(e) =
                source.open_outgoing(flags, open_mode, path, &mut server_end_for_event).await
            {
                // Pass back the channel to propagate the epitaph.
                *server_end = channel::take_channel(&mut server_end_for_event);
                return Err(e);
            }
        }
        Ok(())
    }
}

/// This method gets an optional default capability provider based on the
/// capability source.
fn get_default_provider(
    target: WeakComponentInstance,
    source: &CapabilitySource,
) -> Option<Box<dyn CapabilityProvider>> {
    match source {
        CapabilitySource::Component { capability, component } => {
            // Route normally for a component capability with a source path
            match capability.source_path() {
                Some(path) => Some(Box::new(DefaultComponentCapabilityProvider {
                    target,
                    source: component.clone(),
                    name: capability
                        .source_name()
                        .expect("capability with source path should have a name")
                        .clone(),
                    path: path.clone(),
                })),
                _ => None,
            }
        }
        CapabilitySource::Framework { .. }
        | CapabilitySource::Capability { .. }
        | CapabilitySource::Builtin { .. }
        | CapabilitySource::Namespace { .. } => {
            // There is no default provider for a framework or builtin capability
            None
        }
    }
}

/// Open the capability at the given source, binding to its component instance if necessary.
pub async fn open_capability_at_source(
    flags: u32,
    open_mode: u32,
    relative_path: PathBuf,
    source: CapabilitySource,
    target: &Arc<ComponentInstance>,
    server_chan: &mut zx::Channel,
) -> Result<(), ModelError> {
    target.try_get_context()?.policy().can_route_capability(&source, &target.abs_moniker)?;
    let capability_provider = Arc::new(Mutex::new(get_default_provider(target.as_weak(), &source)));

    let event = Event::new(
        &target,
        Ok(EventPayload::CapabilityRouted {
            source: source.clone(),
            capability_provider: capability_provider.clone(),
        }),
    );
    // Get a capability provider from the tree
    target.hooks.dispatch(&event).await?;

    // This hack changes the flags for a scoped framework service
    let mut flags = flags;
    if let CapabilitySource::Framework { .. } = source {
        flags = SERVICE_OPEN_FLAGS;
    }

    let capability_provider = capability_provider.lock().await.take();

    // If a hook in the component tree gave a capability provider, then use it.
    if let Some(capability_provider) = capability_provider {
        capability_provider.open(flags, open_mode, relative_path, server_chan).await?;
        Ok(())
    } else {
        // TODO(fsamuel): This is a temporary hack. If a global path-based framework capability
        // is not provided by a hook in the component tree, then attempt to connect to the service
        // in component manager's namespace. We could have modeled this as a default provider,
        // but several hooks (such as WorkScheduler) require that a provider is not set.
        let namespace_path = match &source {
            CapabilitySource::Component { .. } => {
                unreachable!(
                    "Capability source is a component, which should have been caught by \
                    default_capability_provider: {:?}",
                    source
                );
            }
            CapabilitySource::Framework { capability, scope_moniker: m } => {
                return Err(RoutingError::capability_from_framework_not_found(
                    &m,
                    capability.source_name().to_string(),
                )
                .into());
            }
            CapabilitySource::Capability { source_capability, component } => {
                return Err(RoutingError::capability_from_capability_not_found(
                    &component.moniker,
                    source_capability.to_string(),
                )
                .into());
            }
            CapabilitySource::Builtin { capability } => {
                return Err(ModelError::from(
                    RoutingError::capability_from_component_manager_not_found(
                        capability.source_name().to_string(),
                    ),
                ));
            }
            CapabilitySource::Namespace { capability } => match capability.source_path() {
                Some(p) => p.clone(),
                _ => {
                    return Err(ModelError::from(
                        RoutingError::capability_from_component_manager_not_found(
                            capability.source_id(),
                        ),
                    ));
                }
            },
        };
        let namespace_path = namespace_path.to_path_buf().attach(relative_path);
        let namespace_path = namespace_path
            .to_str()
            .ok_or_else(|| ModelError::path_is_not_utf8(namespace_path.clone()))?;
        let server_chan = channel::take_channel(server_chan);
        io_util::connect_in_namespace(namespace_path, server_chan, flags).map_err(|e| {
            RoutingError::open_component_manager_namespace_failed(namespace_path, e).into()
        })
    }
}

/// Routes a `UseDecl::Storage` to the component instance providing the backing directory and
/// opens its isolated storage with `server_chan`.
pub async fn route_and_open_storage_capability<'a>(
    use_decl: &'a UseStorageDecl,
    open_mode: u32,
    target: &'a Arc<ComponentInstance>,
    server_chan: &mut zx::Channel,
    bind_reason: &BindReason,
) -> Result<(), ModelError> {
    let (storage_source_info, relative_moniker) =
        route_storage_capability(use_decl, target).await?;
    let dir_source = storage_source_info.storage_provider.clone();
    let relative_moniker_2 = relative_moniker.clone();
    let storage_dir_proxy = storage::open_isolated_storage(
        storage_source_info,
        relative_moniker,
        target.instance_id().as_ref(),
        open_mode,
        bind_reason,
    )
    .await
    .map_err(|e| ModelError::from(e))?;

    // clone the final connection to connect the channel we're routing to its destination
    let server_chan = channel::take_channel(server_chan);
    storage_dir_proxy.clone(fio::CLONE_FLAG_SAME_RIGHTS, ServerEnd::new(server_chan)).map_err(
        |e| {
            let moniker = match &dir_source {
                Some(r) => ExtendedMoniker::ComponentInstance(r.abs_moniker.clone()),
                None => ExtendedMoniker::ComponentManager,
            };
            ModelError::from(RoutingError::open_storage_failed(
                &moniker,
                &relative_moniker_2,
                "",
                e,
            ))
        },
    )?;
    Ok(())
}

/// Routes a `UseDecl::Storage` to the component instance providing the backing directory and
/// deletes its isolated storage.
pub(super) async fn route_and_delete_storage<'a>(
    use_decl: &'a UseStorageDecl,
    target: &'a Arc<ComponentInstance>,
) -> Result<(), ModelError> {
    let (storage_source_info, relative_moniker) =
        route_storage_capability(use_decl, target).await?;
    storage::delete_isolated_storage(
        storage_source_info,
        relative_moniker,
        target.instance_id().as_ref(),
    )
    .await
    .map_err(|e| ModelError::from(e))?;
    Ok(())
}

/// Follows the capability routing for the given use declaration from the target to the
/// storage capability declaration, and then on to the backing directory provider, and returns a
/// `StorageCapabilitySource` containing information discovered durng the routing.
async fn route_storage_capability<'a>(
    use_decl: &'a UseStorageDecl,
    target: &'a Arc<ComponentInstance>,
) -> Result<(storage::StorageCapabilitySource, RelativeMoniker), ModelError> {
    // Walk the offer chain to find the storage decl
    let parent = match target.try_get_parent()? {
        ExtendedInstance::Component(p) => p,
        ExtendedInstance::AboveRoot(_) => {
            return Err(ModelError::from(RoutingError::storage_source_is_not_component(
                "component manager's namespace",
            )));
        }
    };

    let capability = ComponentCapability::Use(UseDecl::Storage(use_decl.clone()));
    let cap_state = CapabilityState::new(&capability);
    let mut pos = WalkPosition {
        capability,
        cap_state,
        last_child_moniker: target.abs_moniker.path().last().map(|c| c.clone()),
        current: ExtendedInstance::Component(parent),
    };

    let source = walk_offer_chain(&mut pos).await?;

    let (storage_decl, source) = match source {
        Some(capability_source) => {
            target
                .try_get_context()?
                .policy()
                .can_route_capability(&capability_source, &target.abs_moniker)?;
            match capability_source {
                CapabilitySource::Component {
                    capability: ComponentCapability::Storage(decl),
                    component,
                } => (decl, component.upgrade()?),
                _ => {
                    unreachable!("Storage capability must come from a storage declaration.");
                }
            }
        }
        _ => {
            unreachable!("Storage capability must come from a storage declaration.");
        }
    };

    let relative_moniker = RelativeMoniker::from_absolute(&source.abs_moniker, &target.abs_moniker);

    Ok((
        route_storage_backing_directory(storage_decl, source, pos.cap_state).await?,
        relative_moniker,
    ))
}

pub async fn route_storage_backing_directory(
    storage_decl: StorageDecl,
    source: Arc<ComponentInstance>,
    mut cap_state: CapabilityState,
) -> Result<storage::StorageCapabilitySource, ModelError> {
    // Find the path and source of the directory consumed by the storage capability.
    let storage_subdir = storage_decl.subdir.clone();
    let (dir_source_path, mut dir_subdir, dir_source) = match storage_decl.source {
        StorageDirectorySource::Self_ => {
            let component_state = source.lock_resolved_state().await?;
            let decl = component_state.decl();
            let capability = ComponentCapability::Storage(storage_decl.clone());
            let capability =
                cap_state.finalize_directory_from_component(&capability, decl, None, None)?;
            let source_path =
                capability.source_path().expect("directory has no source path?").clone();
            drop(component_state); // We can't be holding a reference into source when we move it
            (source_path, None, Some(source))
        }
        StorageDirectorySource::Parent => {
            let capability = ComponentCapability::Storage(storage_decl);
            let (capability_source, cap_state) =
                find_capability_source(capability, &source).await?;
            match capability_source {
                CapabilitySource::Component { capability, component } => {
                    let source_path =
                        capability.source_path().expect("directory has no source path?").clone();
                    let dir_subdir = cap_state.get_subdir().map(Clone::clone);

                    (source_path, dir_subdir, Some(component.upgrade()?))
                }
                CapabilitySource::Framework { .. } => {
                    return Err(RoutingError::storage_directory_source_invalid(
                        "framework",
                        &source.abs_moniker,
                    )
                    .into());
                }
                CapabilitySource::Builtin { .. } => {
                    return Err(RoutingError::storage_directory_source_invalid(
                        "component manager builtin",
                        &source.abs_moniker,
                    )
                    .into());
                }
                CapabilitySource::Capability { .. } => {
                    return Err(RoutingError::storage_directory_source_invalid(
                        "capability",
                        &source.abs_moniker,
                    )
                    .into());
                }
                CapabilitySource::Namespace { capability } => {
                    let source_path =
                        capability.source_path().expect("directory has no source path?").clone();
                    let dir_subdir = cap_state.get_subdir().map(Clone::clone);

                    (source_path, dir_subdir, None)
                }
            }
        }
        StorageDirectorySource::Child(ref name) => {
            let mut pos = {
                let partial = PartialMoniker::new(name.to_string(), None);
                let component_state = source.lock_resolved_state().await?;
                let child = component_state.get_live_child(&partial).ok_or_else(|| {
                    ModelError::from(RoutingError::storage_directory_source_child_not_found(
                        &source.abs_moniker,
                        &partial,
                    ))
                })?;
                let capability = ComponentCapability::Storage(storage_decl);
                WalkPosition {
                    capability,
                    cap_state,
                    last_child_moniker: None,
                    current: ExtendedInstance::Component(child),
                }
            };
            let capability_source = walk_expose_chain(&mut pos).await?;
            match capability_source {
                CapabilitySource::Component { capability, component } => {
                    let source_path =
                        capability.source_path().expect("directory has no source path?").clone();
                    let dir_subdir = pos.cap_state.get_subdir().map(Clone::clone);
                    (source_path, dir_subdir, Some(component.upgrade()?))
                }
                CapabilitySource::Framework { .. } => {
                    return Err(RoutingError::storage_directory_source_invalid(
                        "framework",
                        &source.abs_moniker,
                    )
                    .into());
                }
                CapabilitySource::Capability { .. } => {
                    return Err(RoutingError::storage_directory_source_invalid(
                        "capability",
                        &source.abs_moniker,
                    )
                    .into());
                }
                CapabilitySource::Builtin { .. } | CapabilitySource::Namespace { .. } => {
                    unreachable!(
                        "Invalid capability source for storage with backing dir from child"
                    );
                }
            }
        }
    };
    if dir_subdir == Some(PathBuf::from("")) {
        dir_subdir = None;
    }
    Ok(storage::StorageCapabilitySource {
        storage_provider: dir_source,
        backing_directory_path: dir_source_path,
        backing_directory_subdir: dir_subdir,
        storage_subdir,
    })
}

/// Check if a used capability is from the framework, and if so return a framework
/// `CapabilitySource`.
async fn find_scoped_framework_capability_source<'a>(
    use_decl: &'a UseDecl,
    target: &'a Arc<ComponentInstance>,
) -> Result<Option<CapabilitySource>, ModelError> {
    if let Ok(capability) = InternalCapability::framework_from_use_decl(use_decl) {
        return Ok(Some(CapabilitySource::Framework {
            capability,
            scope_moniker: target.abs_moniker.clone(),
        }));
    }
    return Ok(None);
}

async fn find_use_from_capability_source<'a>(
    use_decl: &'a UseDecl,
    target: &'a Arc<ComponentInstance>,
) -> Result<Option<CapabilitySource>, ModelError> {
    if let UseDecl::Protocol(UseProtocolDecl { source: UseSource::Capability(_), .. }) = use_decl {
        return Ok(Some(CapabilitySource::Capability {
            source_capability: ComponentCapability::Use(use_decl.clone()),
            component: target.as_weak(),
        }));
    }
    Ok(None)
}

/// Holds state about the current position when walking the tree.
#[derive(Debug)]
struct WalkPosition {
    /// The capability declaration as it's represented in the current component.
    capability: ComponentCapability,
    /// Holds any capability-specific state.
    cap_state: CapabilityState,
    /// The moniker of the child we came from.
    last_child_moniker: Option<ChildMoniker>,
    /// The instance we are currently looking at, either a component instance or component manager
    /// itself.
    current: ExtendedInstance,
}

impl WalkPosition {
    fn component(&self) -> &Arc<ComponentInstance> {
        match &self.current {
            ExtendedInstance::Component(r) => &r,
            ExtendedInstance::AboveRoot(_) => {
                panic!("no Component in WalkPosition");
            }
        }
    }

    fn moniker(&self) -> &AbsoluteMoniker {
        match &self.current {
            ExtendedInstance::Component(r) => &r.abs_moniker,
            ExtendedInstance::AboveRoot(_) => {
                panic!("no moniker in WalkPosition");
            }
        }
    }

    fn abs_last_child_moniker(&self) -> AbsoluteMoniker {
        self.moniker().child(self.last_child_moniker.as_ref().expect("no child moniker").clone())
    }

    fn component_manager(&self) -> Option<&Arc<ComponentManagerInstance>> {
        match &self.current {
            ExtendedInstance::Component(_) => None,
            ExtendedInstance::AboveRoot(r) => Some(r),
        }
    }
}

/// Holds state related to a capability when walking the tree
#[derive(Debug, Clone)]
pub enum CapabilityState {
    Directory {
        /// Holds the state of the rights. This is used to enforce directory rights.
        rights_state: WalkState<Rights>,
        /// Holds the subdirectory path to open.
        subdir: PathBuf,
    },
    Event {
        filter_state: WalkState<EventFilter>,
        modes_state: WalkState<EventModeSet>,
    },
    Other,
}

impl CapabilityState {
    pub fn new(cap: &ComponentCapability) -> Self {
        match cap {
            ComponentCapability::Use(UseDecl::Directory(UseDirectoryDecl {
                subdir,
                rights,
                ..
            })) => CapabilityState::Directory {
                rights_state: WalkState::at(Rights::from(*rights)),
                subdir: subdir.as_ref().map_or(PathBuf::new(), |s| PathBuf::from(s)),
            },
            ComponentCapability::Expose(ExposeDecl::Directory(ExposeDirectoryDecl {
                subdir,
                rights,
                ..
            }))
            | ComponentCapability::Offer(OfferDecl::Directory(OfferDirectoryDecl {
                subdir,
                rights,
                ..
            })) => {
                let rights_state = match rights {
                    Some(rights) => WalkState::at(Rights::from(*rights)),
                    None => WalkState::new(),
                };
                CapabilityState::Directory {
                    rights_state,
                    subdir: subdir.as_ref().map_or(PathBuf::new(), |s| PathBuf::from(s)),
                }
            }
            ComponentCapability::Use(UseDecl::Event(UseEventDecl { mode, filter, .. }))
            | ComponentCapability::Offer(OfferDecl::Event(OfferEventDecl {
                mode, filter, ..
            })) => CapabilityState::Event {
                filter_state: WalkState::at(EventFilter::new(filter.clone())),
                modes_state: WalkState::at(EventModeSet::new(mode.clone())),
            },
            ComponentCapability::UsedExpose(ExposeDecl::Directory(ExposeDirectoryDecl {
                ..
            })) => CapabilityState::Directory {
                rights_state: WalkState::new(),
                subdir: PathBuf::new(),
            },
            // Directories backing storage must provide read and write rights.
            ComponentCapability::Use(UseDecl::Storage { .. }) => CapabilityState::Directory {
                rights_state: WalkState::at(Rights::from(*READ_RIGHTS | *WRITE_RIGHTS)),
                subdir: PathBuf::new(),
            },
            ComponentCapability::Storage(_) => CapabilityState::Directory {
                rights_state: WalkState::at(Rights::from(*READ_RIGHTS | *WRITE_RIGHTS)),
                subdir: PathBuf::new(),
            },
            _ => CapabilityState::Other,
        }
    }

    /// Finalize the directory state according to `capability`. Returns a `ComponentCapability` for
    /// the end of routing, or an error if rights did not match.
    ///
    /// REQUIRES: `capability` is a directory expose or offer from `self`.
    fn finalize_directory_from_component(
        &mut self,
        capability: &ComponentCapability,
        decl: &ComponentDecl,
        dir_rights: Option<Rights>,
        subdir_decl: Option<PathBuf>,
    ) -> Result<ComponentCapability, ModelError> {
        if let CapabilityState::Directory { rights_state, subdir } = self {
            *rights_state = rights_state.advance(dir_rights)?;
            CapabilityState::update_subdir(subdir, subdir_decl);
        }

        let directory_decl = capability
            .find_directory_source(decl)
            .expect("directory offer references nonexistent section")
            .clone();
        let dir_rights = Some(Rights::from(directory_decl.rights));
        let capability = ComponentCapability::Directory(directory_decl);
        if let CapabilityState::Directory { rights_state, subdir: _subdir } = self {
            *rights_state = rights_state.finalize(dir_rights)?;
        }
        Ok(capability)
    }

    /// Finalize the directory state for a directory that component from the framework.
    fn finalize_directory_from_framework(
        &mut self,
        subdir_decl: Option<PathBuf>,
    ) -> Result<(), ModelError> {
        if let CapabilityState::Directory { rights_state, subdir } = self {
            *rights_state = rights_state.finalize(Some(Rights::from(*READ_RIGHTS)))?;
            CapabilityState::update_subdir(subdir, subdir_decl);
        }
        Ok(())
    }

    fn make_relative_path(&self, in_relative_path: String) -> PathBuf {
        match self {
            Self::Directory { subdir, .. } => subdir.clone().attach(in_relative_path),
            _ => PathBuf::from(in_relative_path),
        }
    }

    fn update_subdir(subdir: &mut PathBuf, subdir_decl: Option<PathBuf>) {
        let subdir_decl = subdir_decl.unwrap_or(PathBuf::new());
        let old_subdir = subdir.clone();
        *subdir = subdir_decl.attach(old_subdir);
    }

    /// Returns the subdir for the given capability, or None if the subdir is empty. Panics if this
    /// is not a Directory.
    fn get_subdir(&self) -> Option<&PathBuf> {
        match &self {
            CapabilityState::Directory { subdir, .. } if subdir != &PathBuf::new() => Some(subdir),
            CapabilityState::Directory { .. } => None,
            _ => panic!(
                "get_subdir called on a ComponentCapability that is not a Directory: {:?}",
                self
            ),
        }
    }
}

async fn find_used_capability_source<'a>(
    use_decl: &'a UseDecl,
    target: &'a Arc<ComponentInstance>,
) -> Result<(CapabilitySource, CapabilityState), ModelError> {
    let capability = ComponentCapability::Use(use_decl.clone());
    if let Some(framework_capability) =
        find_scoped_framework_capability_source(use_decl, target).await?
    {
        let mut cap_state = CapabilityState::new(&capability);
        match use_decl {
            UseDecl::Service(_) | UseDecl::Protocol(_) | UseDecl::Event(_) => {}
            UseDecl::Directory(_) => {
                cap_state.finalize_directory_from_framework(None)?;
            }
            _ => {
                unreachable!("Invalid framework capability: {:?}", use_decl);
            }
        }
        return Ok((framework_capability, cap_state));
    }
    if let Some((cap_source, cap_state)) =
        find_environment_capability_source(use_decl, target).await?
    {
        return Ok((cap_source, cap_state));
    }
    if let Some(cap_source) = find_use_from_capability_source(use_decl, target).await? {
        let cap_state = CapabilityState::new(&capability);
        return Ok((cap_source, cap_state));
    }
    find_capability_source(capability, target).await
}

/// Attempts to perform capability routing starting from a `use` of a capability that could be
/// provided by an environment.  Returns `None` if `use_decl` is not the type of capability
/// provided by an environment.
async fn find_environment_capability_source<'a>(
    use_decl: &'a UseDecl,
    target: &'a Arc<ComponentInstance>,
) -> Result<Option<(CapabilitySource, CapabilityState)>, ModelError> {
    let cap_state = CapabilityState::new(&ComponentCapability::Use(use_decl.clone()));
    match use_decl {
        UseDecl::Runner(cm_rust::UseRunnerDecl { source_name }) => {
            match target.environment.get_registered_runner(source_name)? {
                Some((Some(env_component), reg)) => {
                    let capability =
                        ComponentCapability::Environment(EnvironmentCapability::Runner {
                            source_name: reg.source_name,
                            source: reg.source.clone(),
                        });
                    Ok(Some(
                        find_environment_component_capability_source(
                            &env_component,
                            capability,
                            cap_state,
                            &reg.source,
                        )
                        .await?,
                    ))
                }
                Some((None, reg)) => {
                    // Root environment.
                    let cap_source = CapabilitySource::Builtin {
                        capability: InternalCapability::Runner(reg.source_name.clone()),
                    };
                    Ok(Some((cap_source, cap_state)))
                }
                None => Err(ModelError::from(RoutingError::use_from_environment_not_found(
                    &target.abs_moniker,
                    "runner",
                    source_name.to_string(),
                ))),
            }
        }
        UseDecl::Protocol(cm_rust::UseProtocolDecl {
            source: cm_rust::UseSource::Debug,
            target_path: _,
            source_name,
        }) => match target.environment.get_debug_capability(source_name)? {
            Some((Some(env_component), reg)) => {
                let capability = ComponentCapability::Environment(EnvironmentCapability::Debug {
                    source_name: reg.source_name,
                    source: reg.source.clone(),
                });
                Ok(Some(
                    find_environment_component_capability_source(
                        &env_component,
                        capability,
                        cap_state,
                        &reg.source,
                    )
                    .await?,
                ))
            }
            Some((None, reg)) => {
                // Root environment.
                let cap_source = CapabilitySource::Builtin {
                    capability: InternalCapability::Protocol(reg.source_name.clone()),
                };
                Ok(Some((cap_source, cap_state)))
            }
            None => Err(ModelError::from(RoutingError::use_from_environment_not_found(
                &target.abs_moniker,
                "debug",
                source_name.to_string(),
            ))),
        },
        _ => Ok(None),
    }
}

async fn find_environment_component_capability_source<'a>(
    env_component: &Arc<ComponentInstance>,
    capability: ComponentCapability,
    cap_state: CapabilityState,
    source: &cm_rust::RegistrationSource,
) -> Result<(CapabilitySource, CapabilityState), ModelError> {
    let env_component_state = env_component.lock_resolved_state().await?;
    let decl = env_component_state.decl();
    match &source {
        cm_rust::RegistrationSource::Self_ => {
            let cap_source = find_capability_source_from_self(&env_component, &capability, decl);
            Ok((cap_source, cap_state))
        }
        cm_rust::RegistrationSource::Parent => {
            let starting_component = env_component.try_get_parent()?;
            let mut pos = WalkPosition {
                capability,
                cap_state,
                last_child_moniker: env_component.abs_moniker.leaf().cloned(),
                current: starting_component,
            };
            if let Some(cap_source) = walk_offer_chain(&mut pos).await? {
                return Ok((cap_source, pos.cap_state));
            }
            let cap_source = walk_expose_chain(&mut pos).await?;
            Ok((cap_source, pos.cap_state))
        }
        cm_rust::RegistrationSource::Child(child_name) => {
            let partial = PartialMoniker::new(child_name.into(), None);
            let component = ExtendedInstance::Component(
                env_component_state.get_live_child(&partial).ok_or_else(|| {
                    ModelError::from(RoutingError::environment_from_child_expose_not_found(
                        &partial,
                        &env_component.abs_moniker,
                        capability.type_name().to_string(),
                        capability.source_id(),
                    ))
                })?,
            );
            let mut pos = WalkPosition {
                capability,
                cap_state,
                last_child_moniker: None,
                current: component,
            };
            let cap_source = walk_expose_chain(&mut pos).await?;
            Ok((cap_source, pos.cap_state))
        }
    }
}

fn find_capability_source_from_self(
    env_component: &Arc<ComponentInstance>,
    capability: &ComponentCapability,
    decl: &cm_rust::ComponentDecl,
) -> CapabilitySource {
    match capability {
        ComponentCapability::Environment(env_cap) => match env_cap {
            EnvironmentCapability::Runner { .. } => {
                let runner_decl =
                    capability.find_runner_source(decl).expect("missing runner").clone();
                CapabilitySource::Component {
                    capability: ComponentCapability::Runner(runner_decl),
                    component: env_component.as_weak(),
                }
            }
            EnvironmentCapability::Resolver { .. } => {
                let resolver_decl =
                    capability.find_resolver_source(decl).expect("missing resolver").clone();
                CapabilitySource::Component {
                    capability: ComponentCapability::Resolver(resolver_decl),
                    component: env_component.as_weak(),
                }
            }
            EnvironmentCapability::Debug { .. } => {
                let protocol_decl =
                    capability.find_protocol_source(decl).expect("missing protocol").clone();
                CapabilitySource::Component {
                    capability: ComponentCapability::Protocol(protocol_decl),
                    component: env_component.as_weak(),
                }
            }
        },
        _ => {
            panic!("Capability has invalid type: {:?}", capability);
        }
    }
}

/// Walks the component tree to return the originating source of a capability, starting on the given
/// abs_moniker, as well as the final capability state.
async fn find_capability_source<'a>(
    capability: ComponentCapability,
    target: &'a Arc<ComponentInstance>,
) -> Result<(CapabilitySource, CapabilityState), ModelError> {
    let starting_component = target.try_get_parent()?;
    let cap_state = CapabilityState::new(&capability);
    let mut pos = WalkPosition {
        capability,
        cap_state,
        last_child_moniker: target.abs_moniker.leaf().cloned(),
        current: starting_component,
    };
    if let Some(source) = walk_offer_chain(&mut pos).await? {
        return Ok((source, pos.cap_state));
    }
    let source = walk_expose_chain(&mut pos).await?;
    Ok((source, pos.cap_state))
}

/// Follows `offer` declarations up the component tree, starting at `pos`. The algorithm looks
/// for a matching `offer` in the parent, as long as the `offer` is from `component`.
///
/// Returns the source of the capability if found, or `None` if `expose` declarations must be
/// walked.
async fn walk_offer_chain<'a>(
    pos: &'a mut WalkPosition,
) -> Result<Option<CapabilitySource>, ModelError> {
    'offerloop: loop {
        if let Some(component_manager) = pos.component_manager() {
            // This is a built-in or namespace capability because the routing path was traced to
            // the component manager's instance.
            match pos.capability.find_namespace_source(&component_manager.namespace_capabilities) {
                Some(CapabilityDecl::Protocol(p)) => {
                    let capability = ComponentCapability::Protocol(p.clone());
                    return Ok(Some(CapabilitySource::Namespace { capability }));
                }
                Some(CapabilityDecl::Directory(d)) => {
                    let dir_rights = Some(Rights::from(d.rights));
                    let capability = ComponentCapability::Directory(d.clone());
                    if let CapabilityState::Directory { rights_state, subdir: _subdir } =
                        &mut pos.cap_state
                    {
                        *rights_state = rights_state.finalize(dir_rights)?;
                    }
                    return Ok(Some(CapabilitySource::Namespace { capability }));
                }
                Some(_) => {
                    return Err(ModelError::unsupported(format!(
                        "Namespace capability not supported: {:?}",
                        pos.capability,
                    )));
                }
                None => (),
            }

            // Not a namespace capability, fallback to builtin.
            let capability = match &pos.capability {
                ComponentCapability::Use(use_decl) => {
                    InternalCapability::builtin_from_use_decl(use_decl).map_err(|_| {
                        ModelError::from(RoutingError::use_from_component_manager_not_found(
                            pos.capability.source_id(),
                        ))
                    })
                }
                ComponentCapability::Offer(offer_decl) => {
                    InternalCapability::builtin_from_offer_decl(offer_decl).map_err(|_| {
                        ModelError::from(RoutingError::offer_from_component_manager_not_found(
                            pos.capability.source_id(),
                        ))
                    })
                }
                ComponentCapability::Storage(storage_decl) => {
                    InternalCapability::builtin_from_storage_decl(storage_decl).map_err(|_| {
                        ModelError::from(RoutingError::storage_from_component_manager_not_found(
                            pos.capability.source_id(),
                        ))
                    })
                }
                _ => Err(ModelError::unsupported(format!(
                    "Built-in capability not supported: {:?}",
                    pos.capability,
                ))),
            }?;
            return Ok(Some(CapabilitySource::Builtin { capability }));
        }

        let cur_component = pos.component().clone();
        let cur_component_state = cur_component.lock_resolved_state().await?;
        let decl = cur_component_state.decl();
        let last_child_moniker = pos.last_child_moniker.as_ref().expect("no child moniker");
        let offer =
            pos.capability.find_offer_source(decl, last_child_moniker).ok_or_else(|| match pos
                .capability
            {
                ComponentCapability::Use(_) => {
                    ModelError::from(RoutingError::use_from_parent_not_found(
                        &pos.abs_last_child_moniker(),
                        pos.capability.source_id(),
                    ))
                }
                ComponentCapability::Environment(_) => {
                    ModelError::from(RoutingError::environment_from_parent_not_found(
                        &pos.abs_last_child_moniker(),
                        pos.capability.type_name().to_string(),
                        pos.capability.source_id(),
                    ))
                }
                ComponentCapability::Offer(_) => {
                    ModelError::from(RoutingError::offer_from_parent_not_found(
                        &pos.abs_last_child_moniker(),
                        pos.capability.source_id(),
                    ))
                }
                ComponentCapability::Storage(_) => {
                    ModelError::from(RoutingError::storage_from_parent_not_found(
                        &pos.abs_last_child_moniker(),
                        pos.capability.source_id(),
                    ))
                }
                _ => {
                    panic!("Invalid offer target: {:?}", pos.capability);
                }
            })?;
        let source = match offer {
            OfferDecl::Service(_) => return Err(ModelError::unsupported("Service capability")),
            OfferDecl::Protocol(s) => OfferSource::Protocol(&s.source),
            OfferDecl::Directory(d) => OfferSource::Directory(&d.source),
            OfferDecl::Storage(s) => OfferSource::Storage(&s.source),
            OfferDecl::Runner(r) => OfferSource::Runner(&r.source),
            OfferDecl::Resolver(r) => OfferSource::Resolver(&r.source),
            OfferDecl::Event(e) => OfferSource::Event(&e.source),
        };
        let (dir_rights, subdir_decl) = match offer {
            OfferDecl::Directory(OfferDirectoryDecl { rights, subdir, .. }) => {
                (rights.map(Rights::from), subdir.clone())
            }
            _ => (None, None),
        };
        let event_filter = Some(EventFilter::new(match offer {
            OfferDecl::Event(OfferEventDecl { filter, .. }) => filter.clone(),
            _ => None,
        }));
        let event_mode = Some(EventModeSet::new(match offer {
            OfferDecl::Event(OfferEventDecl { mode, .. }) => mode.clone(),
            _ => cm_rust::EventMode::Async,
        }));
        match source {
            OfferSource::Service(_) => {
                return Err(ModelError::unsupported("Service capability"));
            }
            OfferSource::Directory(OfferDirectorySource::Framework) => {
                // Directories coming from the framework are limited to
                // read-only rights.
                pos.cap_state.finalize_directory_from_framework(subdir_decl)?;
                let capability = InternalCapability::framework_from_offer_decl(offer)
                    .expect("not a framework offer declaration");
                return Ok(Some(CapabilitySource::Framework {
                    capability,
                    scope_moniker: pos.moniker().clone(),
                }));
            }
            OfferSource::Event(OfferEventSource::Framework) => {
                // An event offered from framework is scoped to the current component.
                if let CapabilityState::Event { modes_state, filter_state } = &mut pos.cap_state {
                    *modes_state = modes_state.finalize(event_mode)?;
                    *filter_state = filter_state.finalize(event_filter)?;
                }
                let capability = InternalCapability::framework_from_offer_decl(offer)
                    .expect("not a framework offer declaration");
                return Ok(Some(CapabilitySource::Framework {
                    capability,
                    scope_moniker: pos.moniker().clone(),
                }));
            }
            OfferSource::Protocol(OfferServiceSource::Parent)
            | OfferSource::Storage(OfferStorageSource::Parent)
            | OfferSource::Runner(OfferRunnerSource::Parent)
            | OfferSource::Resolver(OfferResolverSource::Parent) => {
                // The offered capability comes from the parent, so follow the
                // parent
                pos.capability = ComponentCapability::Offer(offer.clone());
                pos.last_child_moniker = pos.moniker().path().last().map(|c| c.clone());
                pos.current = cur_component.try_get_parent()?;
                continue 'offerloop;
            }
            OfferSource::Event(OfferEventSource::Parent) => {
                // The offered capability comes from the parent, so follow the
                // parent
                if let CapabilityState::Event { modes_state, filter_state } = &mut pos.cap_state {
                    *modes_state = modes_state.advance(event_mode)?;
                    *filter_state = filter_state.advance(event_filter)?;
                }
                pos.capability = ComponentCapability::Offer(offer.clone());
                pos.last_child_moniker = pos.moniker().path().last().map(|c| c.clone());
                pos.current = cur_component.try_get_parent()?;
                continue 'offerloop;
            }
            OfferSource::Directory(OfferDirectorySource::Parent) => {
                if let CapabilityState::Directory { rights_state, subdir } = &mut pos.cap_state {
                    *rights_state = rights_state.advance(dir_rights)?;
                    CapabilityState::update_subdir(subdir, subdir_decl);
                }
                pos.capability = ComponentCapability::Offer(offer.clone());
                pos.last_child_moniker = pos.moniker().path().last().map(|c| c.clone());
                pos.current = cur_component.try_get_parent()?;
                continue 'offerloop;
            }
            OfferSource::Protocol(OfferServiceSource::Self_) => {
                match pos.capability.source_path() {
                    None => {
                        // The offered capability comes from the current component.
                        // Find the current component's Protocol declaration.
                        let cap = ComponentCapability::Offer(offer.clone());
                        return Ok(Some(CapabilitySource::Component {
                            capability: ComponentCapability::Protocol(
                                cap.find_protocol_source(decl)
                                    .expect("protocol offer references nonexistent section")
                                    .clone(),
                            ),
                            component: cur_component.as_weak(),
                        }));
                    }
                    Some(_) => {
                        // Legacy path: protocol is offered by path, get its info from the
                        // OfferDecl.
                        return Ok(Some(CapabilitySource::Component {
                            capability: ComponentCapability::Offer(offer.clone()),
                            component: cur_component.as_weak(),
                        }));
                    }
                }
            }
            OfferSource::Protocol(OfferServiceSource::Capability(_)) => {
                return Ok(Some(CapabilitySource::Capability {
                    source_capability: ComponentCapability::Offer(offer.clone()),
                    component: cur_component.as_weak(),
                }));
            }
            OfferSource::Directory(OfferDirectorySource::Self_) => {
                match pos.capability.source_path() {
                    None => {
                        // The offered capability comes from the current component.  Update state
                        // and return a capability corresponding to the current component's
                        // Directory declaration.
                        let capability = ComponentCapability::Offer(offer.clone());
                        let capability = pos.cap_state.finalize_directory_from_component(
                            &capability,
                            decl,
                            dir_rights,
                            subdir_decl,
                        )?;
                        return Ok(Some(CapabilitySource::Component {
                            capability,
                            component: cur_component.as_weak(),
                        }));
                    }
                    Some(_) => {
                        // Legacy path: directory is offered by path, get its info from the
                        // OfferDecl.
                        if let CapabilityState::Directory { rights_state, subdir } =
                            &mut pos.cap_state
                        {
                            *rights_state = rights_state.finalize(dir_rights)?;
                            CapabilityState::update_subdir(subdir, subdir_decl);
                        }
                        return Ok(Some(CapabilitySource::Component {
                            capability: ComponentCapability::Offer(offer.clone()),
                            component: cur_component.as_weak(),
                        }));
                    }
                }
            }
            OfferSource::Runner(OfferRunnerSource::Self_) => {
                // The offered capability comes from the current component.
                // Find the current component's Runner declaration.
                let cap = ComponentCapability::Offer(offer.clone());
                return Ok(Some(CapabilitySource::Component {
                    capability: ComponentCapability::Runner(
                        cap.find_runner_source(decl)
                            .expect("runner offer references nonexistent section")
                            .clone(),
                    ),
                    component: cur_component.as_weak(),
                }));
            }
            OfferSource::Resolver(OfferResolverSource::Self_) => {
                // The offered capability comes from the current component.
                // Find the current component's Resolver declaration.
                let cap = ComponentCapability::Offer(offer.clone());
                return Ok(Some(CapabilitySource::Component {
                    capability: ComponentCapability::Resolver(
                        cap.find_resolver_source(decl)
                            .expect("resolver offer references nonexistent section")
                            .clone(),
                    ),
                    component: cur_component.as_weak(),
                }));
            }
            OfferSource::Protocol(OfferServiceSource::Child(child_name))
            | OfferSource::Runner(OfferRunnerSource::Child(child_name))
            | OfferSource::Resolver(OfferResolverSource::Child(child_name)) => {
                // The offered capability comes from a child, break the loop
                // and begin walking the expose chain.
                pos.capability = ComponentCapability::Offer(offer.clone());
                let partial = PartialMoniker::new(child_name.to_string(), None);
                pos.current = ExtendedInstance::Component(
                    cur_component_state.get_live_child(&partial).ok_or_else(|| {
                        ModelError::from(RoutingError::offer_from_child_expose_not_found(
                            &partial,
                            pos.moniker(),
                            pos.capability.source_id(),
                        ))
                    })?,
                );
                return Ok(None);
            }
            OfferSource::Directory(OfferDirectorySource::Child(child_name)) => {
                if let CapabilityState::Directory { rights_state, subdir } = &mut pos.cap_state {
                    *rights_state = rights_state.advance(dir_rights)?;
                    CapabilityState::update_subdir(subdir, subdir_decl);
                }
                pos.capability = ComponentCapability::Offer(offer.clone());
                let partial = PartialMoniker::new(child_name.to_string(), None);
                pos.current = ExtendedInstance::Component(
                    cur_component_state.get_live_child(&partial).ok_or(ModelError::from(
                        RoutingError::offer_from_child_instance_not_found(
                            &partial,
                            pos.moniker(),
                            pos.capability.source_id(),
                        ),
                    ))?,
                );
                return Ok(None);
            }
            OfferSource::Storage(OfferStorageSource::Self_) => {
                let storage_name = match &offer {
                    OfferDecl::Storage(s) => &s.source_name,
                    _ => panic!(
                        "OfferSource::Storage on a non-storage OfferDecl should be impossible"
                    ),
                };
                let storage = decl
                    .find_storage_source(storage_name)
                    .expect("storage offer references nonexistent section");
                let capability = ComponentCapability::Storage(storage.clone());
                pos.cap_state = CapabilityState::new(&capability);
                return Ok(Some(CapabilitySource::Component {
                    capability,
                    component: cur_component.as_weak(),
                }));
            }
        }
    }
}

/// Follows `expose` declarations down the component tree, starting at `pos`. The algorithm looks
/// for a matching `expose` in the child, as long as the `expose` is not from `self`.
///
/// Returns the source of the capability.
async fn walk_expose_chain<'a>(pos: &'a mut WalkPosition) -> Result<CapabilitySource, ModelError> {
    loop {
        // TODO(xbhatnag): See if the locking needs to be over the entire loop
        // Consider -> let current_decl = { .. };
        let cur_component = pos.component().clone();
        let cur_component_state = cur_component.lock_resolved_state().await?;
        let decl = cur_component_state.decl();
        let expose =
            pos.capability.find_expose_source(decl).ok_or_else(|| match pos.capability {
                ComponentCapability::UsedExpose(_) => ModelError::from(
                    RoutingError::used_expose_not_found(pos.moniker(), pos.capability.source_id()),
                ),
                ComponentCapability::Environment(_) => {
                    let partial =
                        pos.moniker().leaf().expect("impossible source above root").to_partial();
                    ModelError::from(RoutingError::environment_from_child_expose_not_found(
                        &partial,
                        pos.moniker().parent().as_ref().expect("impossible source above root"),
                        pos.capability.type_name().to_string(),
                        pos.capability.source_id(),
                    ))
                }
                ComponentCapability::Expose(_) => {
                    let partial =
                        pos.moniker().leaf().expect("impossible source above root").to_partial();
                    ModelError::from(RoutingError::expose_from_child_expose_not_found(
                        &partial,
                        pos.moniker().parent().as_ref().expect("impossible source above root"),
                        pos.capability.source_id(),
                    ))
                }
                ComponentCapability::Offer(_) => {
                    let partial =
                        pos.moniker().leaf().expect("impossible source above root").to_partial();
                    ModelError::from(RoutingError::offer_from_child_expose_not_found(
                        &partial,
                        pos.moniker().parent().as_ref().expect("impossible source above root"),
                        pos.capability.source_id(),
                    ))
                }
                ComponentCapability::Storage(_) => {
                    let partial =
                        pos.moniker().leaf().expect("impossible source above root").to_partial();
                    ModelError::from(RoutingError::storage_from_child_expose_not_found(
                        &partial,
                        pos.moniker().parent().as_ref().expect("impossible source above root"),
                        pos.capability.source_id(),
                    ))
                }
                _ => {
                    unreachable!(
                        "Searched for an expose declaration at `{}` for `{}`, but the \
                            source doesn't seem like it should map to an expose declaration",
                        pos.moniker().parent().expect("impossible source above root"),
                        pos.capability.source_id()
                    );
                }
            })?;
        let (source, target) = match expose {
            ExposeDecl::Service(_) => return Err(ModelError::unsupported("Service capability")),
            ExposeDecl::Protocol(ls) => (CapabilityExposeSource::Protocol(&ls.source), &ls.target),
            ExposeDecl::Directory(d) => (CapabilityExposeSource::Directory(&d.source), &d.target),
            ExposeDecl::Runner(r) => (CapabilityExposeSource::Runner(&r.source), &r.target),
            ExposeDecl::Resolver(r) => (CapabilityExposeSource::Resolver(&r.source), &r.target),
        };
        if target != &ExposeTarget::Parent {
            let partial = pos.moniker().leaf().expect("impossible source above root").to_partial();
            return Err(RoutingError::expose_from_child_expose_not_found(
                &partial,
                pos.moniker().parent().as_ref().expect("impossible source above root"),
                pos.capability.source_id(),
            )
            .into());
        }
        let (dir_rights, subdir_decl) = match expose {
            ExposeDecl::Directory(ExposeDirectoryDecl { rights, subdir, .. }) => {
                (rights.map(Rights::from), subdir.clone())
            }
            _ => (None, None),
        };
        match source {
            CapabilityExposeSource::Protocol(ExposeSource::Self_) => {
                match pos.capability.source_path() {
                    None => {
                        // The exposed capability comes from the current component.
                        // Find the current component's Protocol declaration.
                        let cap = ComponentCapability::Expose(expose.clone());
                        return Ok(CapabilitySource::Component {
                            capability: ComponentCapability::Protocol(
                                cap.find_protocol_source(decl)
                                    .expect("protocol offer references nonexistent section")
                                    .clone(),
                            ),
                            component: cur_component.as_weak(),
                        });
                    }
                    Some(_) => {
                        // Legacy path: protocol is exposed by path, get its info from the
                        // ExposeDecl.
                        return Ok(CapabilitySource::Component {
                            capability: ComponentCapability::Expose(expose.clone()),
                            component: cur_component.as_weak(),
                        });
                    }
                }
            }
            CapabilityExposeSource::Protocol(ExposeSource::Capability(_)) => {
                return Ok(CapabilitySource::Capability {
                    source_capability: ComponentCapability::Expose(expose.clone()),
                    component: cur_component.as_weak(),
                });
            }
            CapabilityExposeSource::Directory(ExposeSource::Self_) => {
                match pos.capability.source_path() {
                    None => {
                        // The offered capability comes from the current component.  Update state
                        // and return a capability corresponding to the current component's
                        // Directory declaration.
                        let capability = ComponentCapability::Expose(expose.clone());
                        let capability = pos.cap_state.finalize_directory_from_component(
                            &capability,
                            decl,
                            dir_rights,
                            subdir_decl,
                        )?;
                        return Ok(CapabilitySource::Component {
                            capability,
                            component: cur_component.as_weak(),
                        });
                    }
                    Some(_) => {
                        // Legacy path: directory is exposed by path, get its info from the
                        // ExposeDecl.
                        if let CapabilityState::Directory { rights_state, subdir } =
                            &mut pos.cap_state
                        {
                            *rights_state = rights_state.finalize(dir_rights)?;
                            CapabilityState::update_subdir(subdir, subdir_decl);
                        }
                        return Ok(CapabilitySource::Component {
                            capability: ComponentCapability::Expose(expose.clone()),
                            component: cur_component.as_weak(),
                        });
                    }
                }
            }
            CapabilityExposeSource::Runner(ExposeSource::Self_) => {
                // The exposed capability comes from the current component.
                // Find the current component's Runner declaration.
                let cap = ComponentCapability::Expose(expose.clone());
                return Ok(CapabilitySource::Component {
                    capability: ComponentCapability::Runner(
                        cap.find_runner_source(decl)
                            .expect(&format!(
                                "An `expose from runner` declaration was found at `{}` for `{}`
                                with no corresponding runner declaration. This ComponentDecl should
                                not have passed validation.",
                                pos.moniker(),
                                cap.source_id()
                            ))
                            .clone(),
                    ),
                    component: cur_component.as_weak(),
                });
            }
            CapabilityExposeSource::Resolver(ExposeSource::Self_) => {
                // The exposed capability comes from the current component.
                // Find the current component's Resolver declaration.
                let cap = ComponentCapability::Expose(expose.clone());
                return Ok(CapabilitySource::Component {
                    capability: ComponentCapability::Resolver(
                        cap.find_resolver_source(decl)
                            .expect(&format!(
                                "An `expose from resolver` declaration was found at `{}` for `{}`
                            with no corresponding resolver declaration. This ComponentDecl should
                            not have passed validation.",
                                pos.moniker(),
                                cap.source_id()
                            ))
                            .clone(),
                    ),
                    component: cur_component.as_weak(),
                });
            }
            CapabilityExposeSource::Protocol(ExposeSource::Child(child_name))
            | CapabilityExposeSource::Runner(ExposeSource::Child(child_name))
            | CapabilityExposeSource::Resolver(ExposeSource::Child(child_name)) => {
                // The offered capability comes from a child, so follow the child.
                pos.capability = ComponentCapability::Expose(expose.clone());
                let partial = PartialMoniker::new(child_name.to_string(), None);
                pos.current = ExtendedInstance::Component(
                    cur_component_state.get_live_child(&partial).ok_or(ModelError::from(
                        RoutingError::expose_from_child_instance_not_found(
                            &partial,
                            pos.moniker(),
                            pos.capability.source_id(),
                        ),
                    ))?,
                );
                continue;
            }
            CapabilityExposeSource::Directory(ExposeSource::Child(child_name)) => {
                if let CapabilityState::Directory { rights_state, subdir } = &mut pos.cap_state {
                    *rights_state = rights_state.advance(dir_rights)?;
                    CapabilityState::update_subdir(subdir, subdir_decl);
                }
                // The offered capability comes from a child, so follow the child.
                pos.capability = ComponentCapability::Expose(expose.clone());
                let partial = PartialMoniker::new(child_name.to_string(), None);
                pos.current = ExtendedInstance::Component(
                    cur_component_state.get_live_child(&partial).ok_or(ModelError::from(
                        RoutingError::expose_from_child_instance_not_found(
                            &partial,
                            pos.moniker(),
                            pos.capability.source_id(),
                        ),
                    ))?,
                );
                continue;
            }
            CapabilityExposeSource::Protocol(ExposeSource::Framework) => {
                let capability =
                    InternalCapability::framework_from_expose_decl(expose).map_err(|_| {
                        ModelError::from(RoutingError::expose_from_framework_not_found(
                            pos.moniker(),
                            pos.capability.source_id(),
                        ))
                    })?;
                return Ok(CapabilitySource::Framework {
                    capability,
                    scope_moniker: pos.moniker().clone(),
                });
            }
            CapabilityExposeSource::Directory(ExposeSource::Framework) => {
                // Directories offered or exposed directly from the framework are limited to
                // read-only rights.
                if let CapabilityState::Directory { rights_state, subdir } = &mut pos.cap_state {
                    *rights_state = rights_state.finalize(Some(Rights::from(*READ_RIGHTS)))?;
                    CapabilityState::update_subdir(subdir, subdir_decl);
                }
                let capability =
                    InternalCapability::framework_from_expose_decl(expose).map_err(|_| {
                        ModelError::from(RoutingError::expose_from_framework_not_found(
                            pos.moniker(),
                            pos.capability.source_id(),
                        ))
                    })?;
                return Ok(CapabilitySource::Framework {
                    capability,
                    scope_moniker: pos.moniker().clone(),
                });
            }
            CapabilityExposeSource::Runner(ExposeSource::Capability(_))
            | CapabilityExposeSource::Resolver(ExposeSource::Capability(_))
            | CapabilityExposeSource::Directory(ExposeSource::Capability(_)) => {
                // Currently we don't expose any of these capabilities based on a different
                // capability. This is disallowed in validation, so we should never reach here.
                panic!("non-protocol used a capability as a source");
            }
            CapabilityExposeSource::Runner(ExposeSource::Framework)
            | CapabilityExposeSource::Resolver(ExposeSource::Framework) => {
                // Currently we don't expose any runners/resolvers from `framework`.
                // TODO: This error should be caught by validation. We shouldn't have to handle
                // this case here.
                return Err(RoutingError::expose_from_framework_not_found(
                    pos.moniker(),
                    pos.capability.source_id(),
                )
                .into());
            }
        }
    }
}

/// Sets an epitaph on `server_end` for a capability routing failure, and logs the error. Logs a
/// failure to route a capability. Formats `err` as a `String`, but elides the type if the error is
/// a `RoutingError`, the common case.
pub(super) fn report_routing_failure(
    target_moniker: &AbsoluteMoniker,
    cap: &ComponentCapability,
    err: &ModelError,
    server_end: zx::Channel,
    logger: Option<&dyn FmtArgsLogger>,
) {
    let _ = server_end.close_with_epitaph(routing_epitaph(err));
    let err_str = match err {
        ModelError::RoutingError { err } => format!("{}", err),
        _ => format!("{}", err),
    };
    let log_msg = format!(
        "Failed to route {} `{}` with target component `{}`: {}",
        cap.type_name(),
        cap.source_id(),
        target_moniker,
        err_str
    );
    if let Some(l) = logger {
        l.log(Level::Error, format_args!("{}", log_msg));
    } else {
        MODEL_LOGGER.log(Level::Error, format_args!("{}", log_msg))
    }
}

/// Converts `err` to a `zx::Status` to use as an epitaph on a routed channel.
fn routing_epitaph(err: &ModelError) -> zx::Status {
    match err {
        ModelError::RoutingError { err } => err.as_zx_status(),
        ModelError::RightsError { err } => err.as_zx_status(),
        ModelError::PolicyError { err } => err.as_zx_status(),
        ModelError::InstanceNotFound { .. } => zx::Status::UNAVAILABLE,
        ModelError::Unsupported { .. } => zx::Status::NOT_SUPPORTED,
        // Any other type of error is not expected.
        _ => zx::Status::INTERNAL,
    }
}
