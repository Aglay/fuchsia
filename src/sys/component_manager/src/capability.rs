// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::model::{
        error::ModelError,
        moniker::{AbsoluteMoniker, ChildMoniker},
        realm::WeakRealm,
    },
    async_trait::async_trait,
    cm_rust::*,
    fidl_fuchsia_sys2 as fsys, fuchsia_zircon as zx,
    std::{collections::HashSet, fmt, path::PathBuf},
    thiserror::Error,
};

#[derive(Debug, Error)]
pub enum Error {
    #[error("Invalid framework capability.")]
    InvalidFrameworkCapability {},
    #[error("Invalid builtin capability.")]
    InvalidBuiltinCapability {},
}

/// Describes the source of a capability, as determined by `find_capability_source`
#[derive(Clone, Debug)]
pub enum CapabilitySource {
    /// This capability originates from the component instance for the given Realm.
    /// point.
    Component { capability: ComponentCapability, realm: WeakRealm },
    /// This capability originates from "framework". It's implemented by component manager and is
    /// scoped to the realm of the source.
    Framework { capability: InternalCapability, scope_moniker: AbsoluteMoniker },
    /// This capability originates from the containing realm of the root component. That means it's
    /// built in to component manager or originates from component manager's namespace.
    AboveRoot { capability: InternalCapability },
}

impl CapabilitySource {
    pub fn path(&self) -> Option<&CapabilityPath> {
        match self {
            CapabilitySource::Component { capability, .. } => capability.source_path(),
            CapabilitySource::Framework { capability, .. } => capability.path(),
            CapabilitySource::AboveRoot { capability } => capability.path(),
        }
    }

    pub fn id(&self) -> String {
        match self {
            CapabilitySource::Component { capability, .. } => capability.source_id(),
            CapabilitySource::Framework { capability, .. } => capability.id(),
            CapabilitySource::AboveRoot { capability } => capability.id(),
        }
    }

    pub fn name(&self) -> Option<String> {
        match self {
            CapabilitySource::Component { capability, .. } => {
                capability.source_name().map(|name| name.to_string())
            }
            CapabilitySource::Framework { capability, .. } => {
                capability.name().map(|name| name.to_string())
            }
            CapabilitySource::AboveRoot { capability } => {
                capability.name().map(|name| name.to_string())
            }
        }
    }
}

impl fmt::Display for CapabilitySource {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "{}",
            match self {
                CapabilitySource::Component { capability, realm } => {
                    format!("{} '{}'", capability, realm.moniker)
                }
                CapabilitySource::Framework { capability, .. } => capability.to_string(),
                CapabilitySource::AboveRoot { capability } => capability.to_string(),
            }
        )
    }
}

/// Describes a capability provided by the component manager which could be a framework capability
/// scoped to a realm, a built-in global capability, or a capability from component manager's own
/// namespace.
#[derive(Clone, Debug)]
pub enum InternalCapability {
    Service(CapabilityPath),
    Protocol(CapabilityPath),
    Directory(CapabilityPath),
    Runner(CapabilityName),
    Event(CapabilityName),
}

impl InternalCapability {
    /// Returns a name for the capability type.
    pub fn type_name(&self) -> &'static str {
        match self {
            InternalCapability::Service(_) => "service",
            InternalCapability::Protocol(_) => "protocol",
            InternalCapability::Directory(_) => "directory",
            InternalCapability::Runner(_) => "runner",
            InternalCapability::Event(_) => "event",
        }
    }

    pub fn path(&self) -> Option<&CapabilityPath> {
        match self {
            InternalCapability::Service(source_path) => Some(&source_path),
            InternalCapability::Protocol(source_path) => Some(&source_path),
            InternalCapability::Directory(source_path) => Some(&source_path),
            InternalCapability::Runner(_) | InternalCapability::Event(_) => None,
        }
    }

    pub fn name(&self) -> Option<&CapabilityName> {
        match self {
            InternalCapability::Runner(name) => Some(&name),
            InternalCapability::Event(name) => Some(&name),
            InternalCapability::Service(_)
            | InternalCapability::Protocol(_)
            | InternalCapability::Directory(_) => None,
        }
    }

    pub fn id(&self) -> String {
        self.path()
            .map(|p| format!("{}", p))
            .or_else(|| self.name().map(|n| format!("{}", n)))
            .unwrap_or_default()
    }

    pub fn builtin_from_use_decl(decl: &UseDecl) -> Result<Self, Error> {
        match decl {
            UseDecl::Service(s) if s.source == UseSource::Realm => {
                Ok(InternalCapability::Service(s.source_path.clone()))
            }
            UseDecl::Protocol(s) if s.source == UseSource::Realm => {
                Ok(InternalCapability::Protocol(s.source_path.clone()))
            }
            UseDecl::Directory(d) if d.source == UseSource::Realm => {
                Ok(InternalCapability::Directory(d.source_path.clone()))
            }
            UseDecl::Event(e) if e.source == UseSource::Realm => {
                Ok(InternalCapability::Event(e.source_name.clone()))
            }
            UseDecl::Runner(s) => Ok(InternalCapability::Runner(s.source_name.clone())),
            _ => Err(Error::InvalidBuiltinCapability {}),
        }
    }

    pub fn builtin_from_offer_decl(decl: &OfferDecl) -> Result<Self, Error> {
        match decl {
            OfferDecl::Protocol(s) if s.source == OfferServiceSource::Realm => {
                Ok(InternalCapability::Protocol(s.source_path.clone()))
            }
            OfferDecl::Directory(d) if d.source == OfferDirectorySource::Realm => {
                Ok(InternalCapability::Directory(d.source_path.clone()))
            }
            OfferDecl::Runner(s) if s.source == OfferRunnerSource::Realm => {
                Ok(InternalCapability::Runner(s.source_name.clone()))
            }
            OfferDecl::Event(e) if e.source == OfferEventSource::Realm => {
                Ok(InternalCapability::Event(e.source_name.clone()))
            }
            _ => {
                return Err(Error::InvalidBuiltinCapability {});
            }
        }
    }

    pub fn builtin_from_storage_decl(decl: &StorageDecl) -> Result<Self, Error> {
        if decl.source == StorageDirectorySource::Realm {
            Ok(InternalCapability::Directory(decl.source_path.clone()))
        } else {
            Err(Error::InvalidBuiltinCapability {})
        }
    }

    pub fn framework_from_use_decl(decl: &UseDecl) -> Result<Self, Error> {
        match decl {
            UseDecl::Service(s) if s.source == UseSource::Framework => {
                Ok(InternalCapability::Service(s.source_path.clone()))
            }
            UseDecl::Protocol(s) if s.source == UseSource::Framework => {
                Ok(InternalCapability::Protocol(s.source_path.clone()))
            }
            UseDecl::Directory(d) if d.source == UseSource::Framework => {
                Ok(InternalCapability::Directory(d.source_path.clone()))
            }
            UseDecl::Event(e) if e.source == UseSource::Framework => {
                Ok(InternalCapability::Event(e.source_name.clone()))
            }
            _ => {
                return Err(Error::InvalidFrameworkCapability {});
            }
        }
    }

    pub fn framework_from_offer_decl(decl: &OfferDecl) -> Result<Self, Error> {
        match decl {
            OfferDecl::Protocol(s) if s.source == OfferServiceSource::Realm => {
                Ok(InternalCapability::Protocol(s.source_path.clone()))
            }
            OfferDecl::Directory(d) if d.source == OfferDirectorySource::Framework => {
                Ok(InternalCapability::Directory(d.source_path.clone()))
            }
            OfferDecl::Event(e) if e.source == OfferEventSource::Framework => {
                Ok(InternalCapability::Event(e.source_name.clone()))
            }
            _ => {
                return Err(Error::InvalidFrameworkCapability {});
            }
        }
    }

    pub fn framework_from_expose_decl(decl: &ExposeDecl) -> Result<Self, Error> {
        match decl {
            ExposeDecl::Protocol(d) if d.source == ExposeSource::Framework => {
                Ok(InternalCapability::Protocol(d.source_path.clone()))
            }
            ExposeDecl::Directory(d) if d.source == ExposeSource::Framework => {
                Ok(InternalCapability::Directory(d.source_path.clone()))
            }
            _ => {
                return Err(Error::InvalidFrameworkCapability {});
            }
        }
    }
}

impl fmt::Display for InternalCapability {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{} '{}' from framework", self.type_name(), self.id())
    }
}

/// The server-side of a capability implements this trait.
/// Multiple `CapabilityProvider` objects can compose with one another for a single
/// capability request. For example, a `CapabitilityProvider` can be interposed
/// between the primary `CapabilityProvider and the client for the purpose of
/// logging and testing. A `CapabilityProvider` is typically provided by a
/// corresponding `Hook` in response to the `CapabilityRouted` event.
/// A capability provider is used exactly once as a result of exactly one route.
#[async_trait]
pub trait CapabilityProvider: Send + Sync {
    // Called to bind a server end of a zx::Channel to the provided capability.
    // If the capability is a directory, then |flags|, |open_mode| and |relative_path|
    // will be propagated along to open the appropriate directory.
    async fn open(
        self: Box<Self>,
        flags: u32,
        open_mode: u32,
        relative_path: PathBuf,
        server_end: &mut zx::Channel,
    ) -> Result<(), ModelError>;
}

/// A capability being routed from a component.
#[derive(Clone, Debug)]
pub enum ComponentCapability {
    Use(UseDecl),
    /// Models a capability used from the environment.
    Environment(EnvironmentCapability),
    Expose(ExposeDecl),
    /// Models a capability hosted from the exposed dir which is used at runtime.
    UsedExpose(ExposeDecl),
    Offer(OfferDecl),
    Storage(StorageDecl),
    Runner(RunnerDecl),
}

impl ComponentCapability {
    /// Returns a name for the capability type.
    pub fn type_name(&self) -> &'static str {
        match self {
            ComponentCapability::Use(use_) => match use_ {
                UseDecl::Protocol(_) => "protocol",
                UseDecl::Directory(_) => "directory",
                UseDecl::Service(_) => "service",
                UseDecl::Storage(_) => "storage",
                UseDecl::Runner(_) => "runner",
                UseDecl::Event(_) => "event",
                UseDecl::EventStream(_) => "event_stream",
            },
            ComponentCapability::Environment(env) => match env {
                EnvironmentCapability::Runner { .. } => "runner",
            },
            ComponentCapability::Expose(expose) | ComponentCapability::UsedExpose(expose) => {
                match expose {
                    ExposeDecl::Protocol(_) => "protocol",
                    ExposeDecl::Directory(_) => "directory",
                    ExposeDecl::Service(_) => "service",
                    ExposeDecl::Runner(_) => "runner",
                    ExposeDecl::Resolver(_) => "resolver",
                }
            }
            ComponentCapability::Offer(offer) => match offer {
                OfferDecl::Protocol(_) => "protocol",
                OfferDecl::Directory(_) => "directory",
                OfferDecl::Service(_) => "service",
                OfferDecl::Storage(_) => "storage",
                OfferDecl::Runner(_) => "runner",
                OfferDecl::Resolver(_) => "resolver",
                OfferDecl::Event(_) => "event",
            },
            ComponentCapability::Storage(_) => "storage",
            ComponentCapability::Runner(_) => "runner",
        }
    }

    /// Returns the source path of the capability, if one exists.
    pub fn source_path(&self) -> Option<&CapabilityPath> {
        match self {
            ComponentCapability::Use(use_) => match use_ {
                UseDecl::Protocol(UseProtocolDecl { source_path, .. }) => Some(source_path),
                UseDecl::Directory(UseDirectoryDecl { source_path, .. }) => Some(source_path),
                _ => None,
            },
            ComponentCapability::Environment(env_cap) => match env_cap {
                EnvironmentCapability::Runner { .. } => None,
            },
            ComponentCapability::Expose(expose) => match expose {
                ExposeDecl::Protocol(ExposeProtocolDecl { source_path, .. }) => Some(source_path),
                ExposeDecl::Directory(ExposeDirectoryDecl { source_path, .. }) => Some(source_path),
                _ => None,
            },
            ComponentCapability::UsedExpose(expose) => {
                // A UsedExpose needs to be matched to the ExposeDecl the UsedExpose wraps at the
                // same component. This is accomplished by returning the ExposeDecl's target path.
                // Effectively, it's as if the UsedExposed were a UseDecl with both the source and
                // target path equal to `target_path`.
                match expose {
                    ExposeDecl::Protocol(ExposeProtocolDecl { target_path, .. }) => {
                        Some(target_path)
                    }
                    ExposeDecl::Directory(ExposeDirectoryDecl { target_path, .. }) => {
                        Some(target_path)
                    }
                    _ => None,
                }
            }
            ComponentCapability::Offer(offer) => match offer {
                OfferDecl::Protocol(OfferProtocolDecl { source_path, .. }) => Some(source_path),
                OfferDecl::Directory(OfferDirectoryDecl { source_path, .. }) => Some(source_path),
                _ => None,
            },
            ComponentCapability::Runner(RunnerDecl { source_path, .. }) => Some(source_path),
            ComponentCapability::Storage(_) => None,
        }
    }

    /// Return the source name of the capability, if one exists.
    pub fn source_name<'a>(&self) -> Option<&CapabilityName> {
        match self {
            ComponentCapability::Use(use_) => match use_ {
                UseDecl::Runner(UseRunnerDecl { source_name, .. }) => Some(source_name),
                UseDecl::Event(UseEventDecl { source_name, .. }) => Some(source_name),
                UseDecl::Storage(d) => Some(d.type_name()),
                _ => None,
            },
            ComponentCapability::Environment(env_cap) => match env_cap {
                EnvironmentCapability::Runner { source_name, .. } => Some(source_name),
            },
            ComponentCapability::Expose(expose) => match expose {
                ExposeDecl::Runner(ExposeRunnerDecl { source_name, .. }) => Some(source_name),
                _ => None,
            },
            ComponentCapability::Offer(offer) => match offer {
                OfferDecl::Runner(OfferRunnerDecl { source_name, .. }) => Some(source_name),
                OfferDecl::Event(OfferEventDecl { source_name, .. }) => Some(source_name),
                OfferDecl::Storage(d) => Some(d.type_name()),
                _ => None,
            },
            _ => None,
        }
    }

    /// Returns the source path or name of the capability as a string, useful for debugging.
    pub fn source_id(&self) -> String {
        self.source_path()
            .map(|p| format!("{}", p))
            .or_else(|| self.source_name().map(|n| format!("{}", n)))
            .unwrap_or_default()
    }

    /// Returns the `ExposeDecl` that exposes the capability, if it exists.
    pub fn find_expose_source<'a>(&self, decl: &'a ComponentDecl) -> Option<&'a ExposeDecl> {
        decl.exposes.iter().find(|&expose| match (self, expose) {
            // Protocol exposed to me that has a matching `expose` or `offer`.
            (
                ComponentCapability::Offer(OfferDecl::Protocol(parent_offer)),
                ExposeDecl::Protocol(expose),
            ) => parent_offer.source_path == expose.target_path,
            (
                ComponentCapability::Expose(ExposeDecl::Protocol(parent_expose)),
                ExposeDecl::Protocol(expose),
            ) => parent_expose.source_path == expose.target_path,
            (
                ComponentCapability::UsedExpose(ExposeDecl::Protocol(used_expose)),
                ExposeDecl::Protocol(expose),
            ) => used_expose.target_path == expose.target_path,
            // Directory exposed to me that matches a directory `expose` or `offer`.
            (
                ComponentCapability::Offer(OfferDecl::Directory(parent_offer)),
                ExposeDecl::Directory(expose),
            ) => parent_offer.source_path == expose.target_path,
            (
                ComponentCapability::Expose(ExposeDecl::Directory(parent_expose)),
                ExposeDecl::Directory(expose),
            ) => parent_expose.source_path == expose.target_path,
            (
                ComponentCapability::UsedExpose(ExposeDecl::Directory(used_expose)),
                ExposeDecl::Directory(expose),
            ) => used_expose.target_path == expose.target_path,
            // Runner exposed to me that has a matching `expose` or `offer`.
            (
                ComponentCapability::Offer(OfferDecl::Runner(parent_offer)),
                ExposeDecl::Runner(expose),
            ) => parent_offer.source_name == expose.target_name,
            (
                ComponentCapability::Expose(ExposeDecl::Runner(parent_expose)),
                ExposeDecl::Runner(expose),
            ) => parent_expose.source_name == expose.target_name,
            (
                ComponentCapability::Environment(EnvironmentCapability::Runner {
                    source_name, ..
                }),
                ExposeDecl::Runner(expose),
            ) => source_name == &expose.target_name,
            // Directory exposed to me that matches a `storage` declaration which consumes it.
            (ComponentCapability::Storage(parent_storage), ExposeDecl::Directory(expose)) => {
                parent_storage.source_path == expose.target_path
            }
            _ => false,
        })
    }

    /// Returns the set of `ExposeServiceDecl`s that expose the service capability, if they exist.
    #[allow(unused)]
    pub fn find_expose_service_sources<'a>(
        &self,
        decl: &'a ComponentDecl,
    ) -> Vec<&'a ExposeServiceDecl> {
        let paths: HashSet<_> = match self {
            ComponentCapability::Offer(OfferDecl::Service(parent_offer)) => {
                parent_offer.sources.iter().map(|s| &s.source_path).collect()
            }
            ComponentCapability::Expose(ExposeDecl::Service(parent_expose)) => {
                parent_expose.sources.iter().map(|s| &s.source_path).collect()
            }
            _ => panic!("Expected an offer or expose of a service capability, found: {:?}", self),
        };
        decl.exposes
            .iter()
            .filter_map(|expose| match expose {
                ExposeDecl::Service(expose) if paths.contains(&expose.target_path) => Some(expose),
                _ => None,
            })
            .collect()
    }

    /// Given a parent ComponentDecl, returns the `OfferDecl` that offers this capability to
    /// `child_moniker`, if it exists.
    pub fn find_offer_source<'a>(
        &self,
        decl: &'a ComponentDecl,
        child_moniker: &ChildMoniker,
    ) -> Option<&'a OfferDecl> {
        decl.offers.iter().find(|&offer| {
            match (self, offer) {
                // Protocol offered to me that matches a service `use` or `offer` declaration.
                (
                    ComponentCapability::Use(UseDecl::Protocol(child_use)),
                    OfferDecl::Protocol(offer),
                ) => Self::is_offer_protocol_or_directory_match(
                    child_moniker,
                    &child_use.source_path,
                    &offer.target,
                    &offer.target_path,
                ),
                (
                    ComponentCapability::Offer(OfferDecl::Protocol(child_offer)),
                    OfferDecl::Protocol(offer),
                ) => Self::is_offer_protocol_or_directory_match(
                    child_moniker,
                    &child_offer.source_path,
                    &offer.target,
                    &offer.target_path,
                ),
                // Directory offered to me that matches a directory `use` or `offer` declaration.
                (
                    ComponentCapability::Use(UseDecl::Directory(child_use)),
                    OfferDecl::Directory(offer),
                ) => Self::is_offer_protocol_or_directory_match(
                    child_moniker,
                    &child_use.source_path,
                    &offer.target,
                    &offer.target_path,
                ),
                (
                    ComponentCapability::Offer(OfferDecl::Directory(child_offer)),
                    OfferDecl::Directory(offer),
                ) => Self::is_offer_protocol_or_directory_match(
                    child_moniker,
                    &child_offer.source_path,
                    &offer.target,
                    &offer.target_path,
                ),
                // Directory offered to me that matches a `storage` declaration which consumes it.
                (ComponentCapability::Storage(child_storage), OfferDecl::Directory(offer)) => {
                    Self::is_offer_protocol_or_directory_match(
                        child_moniker,
                        &child_storage.source_path,
                        &offer.target,
                        &offer.target_path,
                    )
                }
                // Storage offered to me.
                (
                    ComponentCapability::Use(UseDecl::Storage(child_use)),
                    OfferDecl::Storage(offer),
                ) => Self::is_offer_storage_match(
                    child_moniker,
                    child_use.type_(),
                    offer.target(),
                    offer.type_(),
                ),
                (
                    ComponentCapability::Offer(OfferDecl::Storage(child_offer)),
                    OfferDecl::Storage(offer),
                ) => Self::is_offer_storage_match(
                    child_moniker,
                    child_offer.type_(),
                    offer.target(),
                    offer.type_(),
                ),
                // Runners offered from parent.
                (
                    ComponentCapability::Use(UseDecl::Runner(child_use)),
                    OfferDecl::Runner(offer),
                ) => Self::is_offer_runner_or_event_match(
                    child_moniker,
                    &child_use.source_name,
                    &offer.target,
                    &offer.target_name,
                ),
                (
                    ComponentCapability::Offer(OfferDecl::Runner(child_offer)),
                    OfferDecl::Runner(offer),
                ) => Self::is_offer_runner_or_event_match(
                    child_moniker,
                    &child_offer.source_name,
                    &offer.target,
                    &offer.target_name,
                ),
                (
                    ComponentCapability::Environment(EnvironmentCapability::Runner {
                        source_name,
                        ..
                    }),
                    OfferDecl::Runner(offer),
                ) => Self::is_offer_runner_or_event_match(
                    child_moniker,
                    &source_name,
                    &offer.target,
                    &offer.target_name,
                ),
                // Events offered from parent.
                (ComponentCapability::Use(UseDecl::Event(child_use)), OfferDecl::Event(offer)) => {
                    Self::is_offer_runner_or_event_match(
                        child_moniker,
                        &child_use.source_name,
                        &offer.target,
                        &offer.target_name,
                    )
                }
                (
                    ComponentCapability::Offer(OfferDecl::Event(child_offer)),
                    OfferDecl::Event(parent_offer),
                ) => Self::is_offer_runner_or_event_match(
                    child_moniker,
                    &child_offer.source_name,
                    &parent_offer.target,
                    &parent_offer.target_name,
                ),
                _ => false,
            }
        })
    }

    /// Returns the set of `OfferServiceDecl`s that offer the service capability, if they exist.
    #[allow(unused)]
    pub fn find_offer_service_sources<'a>(
        &self,
        decl: &'a ComponentDecl,
        child_moniker: &ChildMoniker,
    ) -> Vec<&'a OfferServiceDecl> {
        let paths: HashSet<_> = match self {
            ComponentCapability::Use(UseDecl::Service(child_use)) => {
                vec![&child_use.source_path].into_iter().collect()
            }
            ComponentCapability::Offer(OfferDecl::Service(child_offer)) => {
                child_offer.sources.iter().map(|s| &s.source_path).collect()
            }
            _ => panic!("Expected a use or offer of a service capability, found: {:?}", self),
        };
        decl.offers
            .iter()
            .filter_map(|offer| match offer {
                OfferDecl::Service(offer)
                    if Self::is_offer_service_match(
                        child_moniker,
                        &paths,
                        &offer.target,
                        &offer.target_path,
                    ) =>
                {
                    Some(offer)
                }
                _ => None,
            })
            .collect()
    }

    /// Given a offer/expose of a Runner from `Self`, return the associated RunnerDecl,
    /// if it exists.
    pub fn find_runner_source<'a>(&self, decl: &'a ComponentDecl) -> Option<&'a RunnerDecl> {
        decl.find_runner_source(self.source_name()?)
    }

    fn is_offer_service_match(
        child_moniker: &ChildMoniker,
        paths: &HashSet<&CapabilityPath>,
        target: &OfferTarget,
        target_path: &CapabilityPath,
    ) -> bool {
        paths.contains(target_path) && target_matches_moniker(target, child_moniker)
    }

    fn is_offer_protocol_or_directory_match(
        child_moniker: &ChildMoniker,
        path: &CapabilityPath,
        target: &OfferTarget,
        target_path: &CapabilityPath,
    ) -> bool {
        path == target_path && target_matches_moniker(target, child_moniker)
    }

    fn is_offer_storage_match(
        child_moniker: &ChildMoniker,
        child_type: fsys::StorageType,
        parent_target: &OfferTarget,
        parent_type: fsys::StorageType,
    ) -> bool {
        // The types must match...
        parent_type == child_type &&
        // ...and the child/collection names must match.
        target_matches_moniker(parent_target, child_moniker)
    }

    fn is_offer_runner_or_event_match(
        child_moniker: &ChildMoniker,
        source_name: &CapabilityName,
        target: &OfferTarget,
        target_name: &CapabilityName,
    ) -> bool {
        source_name == target_name && target_matches_moniker(target, child_moniker)
    }
}

impl fmt::Display for ComponentCapability {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{} '{}' from component", self.type_name(), self.source_id())
    }
}

/// Returns if `parent_target` refers to a the child `child_moniker`.
fn target_matches_moniker(parent_target: &OfferTarget, child_moniker: &ChildMoniker) -> bool {
    match (parent_target, child_moniker.collection()) {
        (OfferTarget::Child(target_child_name), None) => target_child_name == child_moniker.name(),
        (OfferTarget::Collection(target_collection_name), Some(collection)) => {
            target_collection_name == collection
        }
        _ => false,
    }
}

#[derive(Clone, Debug)]
pub enum EnvironmentCapability {
    Runner { source_name: CapabilityName, source: RegistrationSource },
}

#[cfg(test)]
mod tests {
    use cm_rust::OfferServiceDecl;
    use {
        super::*,
        crate::model::testing::test_helpers::default_component_decl,
        cm_rust::{
            ExposeRunnerDecl, ExposeSource, ExposeTarget, OfferServiceSource, ServiceSource,
            StorageDirectorySource, UseRunnerDecl,
        },
    };

    #[test]
    fn find_expose_service_sources() {
        let capability = ComponentCapability::Expose(ExposeDecl::Service(ExposeServiceDecl {
            sources: vec![
                ServiceSource {
                    source: ExposeServiceSource::Self_,
                    source_path: CapabilityPath {
                        dirname: "/svc".to_string(),
                        basename: "net".to_string(),
                    },
                },
                ServiceSource {
                    source: ExposeServiceSource::Self_,
                    source_path: CapabilityPath {
                        dirname: "/svc".to_string(),
                        basename: "log".to_string(),
                    },
                },
                ServiceSource {
                    source: ExposeServiceSource::Self_,
                    source_path: CapabilityPath {
                        dirname: "/svc".to_string(),
                        basename: "unmatched-source".to_string(),
                    },
                },
            ],
            target: ExposeTarget::Realm,
            target_path: CapabilityPath { dirname: "".to_string(), basename: "".to_string() },
        }));
        let net_service = ExposeServiceDecl {
            sources: vec![],
            target: ExposeTarget::Realm,
            target_path: CapabilityPath {
                dirname: "/svc".to_string(),
                basename: "net".to_string(),
            },
        };
        let log_service = ExposeServiceDecl {
            sources: vec![],
            target: ExposeTarget::Realm,
            target_path: CapabilityPath {
                dirname: "/svc".to_string(),
                basename: "log".to_string(),
            },
        };
        let unmatched_service = ExposeServiceDecl {
            sources: vec![],
            target: ExposeTarget::Realm,
            target_path: CapabilityPath {
                dirname: "/svc".to_string(),
                basename: "unmatched-target".to_string(),
            },
        };
        let decl = ComponentDecl {
            exposes: vec![
                ExposeDecl::Service(net_service.clone()),
                ExposeDecl::Service(log_service.clone()),
                ExposeDecl::Service(unmatched_service.clone()),
            ],
            ..default_component_decl()
        };
        let sources = capability.find_expose_service_sources(&decl);
        assert_eq!(sources, vec![&net_service, &log_service])
    }

    #[test]
    #[should_panic]
    #[ignore] // fxb/40189
    fn find_expose_service_sources_with_unexpected_capability() {
        let capability = ComponentCapability::Storage(StorageDecl {
            name: "".to_string(),
            source: StorageDirectorySource::Realm,
            source_path: CapabilityPath { dirname: "".to_string(), basename: "".to_string() },
        });
        capability.find_expose_service_sources(&default_component_decl());
    }

    #[test]
    fn find_offer_service_sources() {
        let capability = ComponentCapability::Offer(OfferDecl::Service(OfferServiceDecl {
            sources: vec![
                ServiceSource {
                    source: OfferServiceSource::Self_,
                    source_path: CapabilityPath {
                        dirname: "/svc".to_string(),
                        basename: "net".to_string(),
                    },
                },
                ServiceSource {
                    source: OfferServiceSource::Self_,
                    source_path: CapabilityPath {
                        dirname: "/svc".to_string(),
                        basename: "log".to_string(),
                    },
                },
                ServiceSource {
                    source: OfferServiceSource::Self_,
                    source_path: CapabilityPath {
                        dirname: "/svc".to_string(),
                        basename: "unmatched-source".to_string(),
                    },
                },
            ],
            target: OfferTarget::Child("".to_string()),
            target_path: CapabilityPath { dirname: "".to_string(), basename: "".to_string() },
        }));
        let net_service = OfferServiceDecl {
            sources: vec![],
            target: OfferTarget::Child("child".to_string()),
            target_path: CapabilityPath {
                dirname: "/svc".to_string(),
                basename: "net".to_string(),
            },
        };
        let log_service = OfferServiceDecl {
            sources: vec![],
            target: OfferTarget::Child("child".to_string()),
            target_path: CapabilityPath {
                dirname: "/svc".to_string(),
                basename: "log".to_string(),
            },
        };
        let unmatched_service = OfferServiceDecl {
            sources: vec![],
            target: OfferTarget::Child("child".to_string()),
            target_path: CapabilityPath {
                dirname: "/svc".to_string(),
                basename: "unmatched-target".to_string(),
            },
        };
        let decl = ComponentDecl {
            offers: vec![
                OfferDecl::Service(net_service.clone()),
                OfferDecl::Service(log_service.clone()),
                OfferDecl::Service(unmatched_service.clone()),
            ],
            ..default_component_decl()
        };
        let moniker = ChildMoniker::new("child".to_string(), None, 0);
        let sources = capability.find_offer_service_sources(&decl, &moniker);
        assert_eq!(sources, vec![&net_service, &log_service])
    }

    #[test]
    fn find_offer_source_runner() {
        // Parents offers runner named "elf" to "child".
        let parent_decl = ComponentDecl {
            offers: vec![
                // Offer as "elf" to child "child".
                OfferDecl::Runner(cm_rust::OfferRunnerDecl {
                    source: cm_rust::OfferRunnerSource::Self_,
                    source_name: "source".into(),
                    target: cm_rust::OfferTarget::Child("child".to_string()),
                    target_name: "elf".into(),
                }),
            ],
            ..default_component_decl()
        };

        // A child named "child" uses a runner "elf" offered by its parent. Should successfully
        // match the declaration.
        let child_cap =
            ComponentCapability::Use(UseDecl::Runner(UseRunnerDecl { source_name: "elf".into() }));
        assert_eq!(
            child_cap.find_offer_source(&parent_decl, &"child:0".into()),
            Some(&parent_decl.offers[0])
        );

        // Mismatched child name.
        assert_eq!(child_cap.find_offer_source(&parent_decl, &"other-child:0".into()), None);

        // Mismatched cap name.
        let misnamed_child_cap = ComponentCapability::Use(UseDecl::Runner(UseRunnerDecl {
            source_name: "dwarf".into(),
        }));
        assert_eq!(misnamed_child_cap.find_offer_source(&parent_decl, &"child:0".into()), None);
    }

    #[test]
    fn find_offer_source_event() {
        // Parent offers event named "started" to "child"
        let parent_decl = ComponentDecl {
            offers: vec![OfferDecl::Event(cm_rust::OfferEventDecl {
                source: cm_rust::OfferEventSource::Realm,
                source_name: "started".into(),
                target: cm_rust::OfferTarget::Child("child".to_string()),
                target_name: "started".into(),
                filter: None,
            })],
            ..default_component_decl()
        };

        // A child named "child" uses the event "started" offered by its parent. Should
        // successfully match the declaration.
        let child_cap = ComponentCapability::Use(UseDecl::Event(UseEventDecl {
            source: cm_rust::UseSource::Realm,
            source_name: "started".into(),
            target_name: "started-x".into(),
            filter: None,
        }));

        assert_eq!(
            child_cap.find_offer_source(&parent_decl, &"child:0".into()),
            Some(&parent_decl.offers[0])
        );

        // Mismatched child name.
        assert_eq!(child_cap.find_offer_source(&parent_decl, &"other-child:0".into()), None);

        // Mismatched capability name.
        let misnamed_child_cap = ComponentCapability::Use(UseDecl::Event(UseEventDecl {
            source: cm_rust::UseSource::Realm,
            source_name: "foo".into(),
            target_name: "started".into(),
            filter: None,
        }));
        assert_eq!(misnamed_child_cap.find_offer_source(&parent_decl, &"child:0".into()), None);
    }

    #[test]
    fn find_expose_source_runner() {
        // A child named "child" exposes a runner "elf" to its parent.
        let child_decl = ComponentDecl {
            exposes: vec![
                // Expose as "elf" to Realm.
                ExposeDecl::Runner(cm_rust::ExposeRunnerDecl {
                    source: cm_rust::ExposeSource::Self_,
                    source_name: "source".into(),
                    target: cm_rust::ExposeTarget::Realm,
                    target_name: "elf".into(),
                }),
            ],
            ..default_component_decl()
        };

        // A parent exposes a runner "elf" with a child as its source. Should successfully match the
        // declaration.
        let parent_cap = ComponentCapability::Expose(ExposeDecl::Runner(ExposeRunnerDecl {
            source: ExposeSource::Child("child".into()),
            source_name: "elf".into(),
            target: ExposeTarget::Realm,
            target_name: "parent_elf".into(),
        }));
        assert_eq!(parent_cap.find_expose_source(&child_decl), Some(&child_decl.exposes[0]));

        // If the name is mismatched, we shouldn't find anything though.
        let misnamed_parent_cap =
            ComponentCapability::Expose(ExposeDecl::Runner(ExposeRunnerDecl {
                source: ExposeSource::Child("child".into()),
                source_name: "dwarf".into(),
                target: ExposeTarget::Realm,
                target_name: "parent_elf".into(),
            }));
        assert_eq!(misnamed_parent_cap.find_expose_source(&child_decl), None);
    }

    #[test]
    #[should_panic]
    #[ignore] // fxb/40189
    fn find_offer_service_sources_with_unexpected_capability() {
        let capability = ComponentCapability::Storage(StorageDecl {
            name: "".to_string(),
            source: StorageDirectorySource::Realm,
            source_path: CapabilityPath { dirname: "".to_string(), basename: "".to_string() },
        });
        let moniker = ChildMoniker::new("".to_string(), None, 0);
        capability.find_offer_service_sources(&default_component_decl(), &moniker);
    }
}
