// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::model::*,
    cm_rust::{self, Capability, ExposeSource, OfferSource},
    failure::format_err,
    fidl_fuchsia_io::{MODE_TYPE_DIRECTORY, MODE_TYPE_SERVICE, OPEN_RIGHT_READABLE},
    fuchsia_zircon as zx,
    futures::lock::Mutex,
    io_util,
    std::sync::Arc,
};

/// Describes the source of a capability, as determined by `find_capability_source`
#[derive(Debug)]
enum CapabilitySource {
    /// This capability originates from the component instance for the given Realm.
    /// point.
    Component(Capability, Arc<Mutex<Realm>>),
    /// This capability originates from component manager's namespace.
    ComponentMgrNamespace(Capability),
}

/// `route_directory` will find the source of the directory capability used at `source_path` by
/// `absolute_moniker`, and pass along the `server_chan` to the hosting component's out
/// directory (or componentmgr's namespace, if applicable)
pub async fn route_directory<'a>(
    model: &'a Model,
    capability: &'a Capability,
    abs_moniker: AbsoluteMoniker,
    server_chan: zx::Channel,
) -> Result<(), ModelError> {
    await!(route_capability(model, MODE_TYPE_DIRECTORY, capability, abs_moniker, server_chan))
}

/// `route_service` will find the source of the service capability used at `source_path` by
/// `absolute_moniker`, and pass along the `server_chan` to the hosting component's out
/// directory (or componentmgr's namespace, if applicable)
pub async fn route_service<'a>(
    model: &'a Model,
    capability: &'a Capability,
    abs_moniker: AbsoluteMoniker,
    server_chan: zx::Channel,
) -> Result<(), ModelError> {
    await!(route_capability(model, MODE_TYPE_SERVICE, capability, abs_moniker, server_chan))
}

/// `route_capability` will find the source of the capability `type_` used at `source_path` by
/// `absolute_moniker`, and pass along the `server_chan` to the hosting component's out
/// directory (or componentmgr's namespace, if applicable) using an open request with
/// `open_mode`.
async fn route_capability<'a>(
    model: &'a Model,
    open_mode: u32,
    capability: &'a Capability,
    abs_moniker: AbsoluteMoniker,
    server_chan: zx::Channel,
) -> Result<(), ModelError> {
    let source = await!(find_capability_source(model, capability, abs_moniker))?;

    let flags = OPEN_RIGHT_READABLE;
    match source {
        CapabilitySource::ComponentMgrNamespace(source_capability) => {
            io_util::connect_in_namespace(&source_capability.path().to_string(), server_chan)
                .map_err(|e| ModelError::capability_discovery_error(e))?
        }
        CapabilitySource::Component(source_capability, realm) => {
            await!(Model::bind_instance_and_open(
                &model,
                realm,
                flags,
                open_mode,
                source_capability.path(),
                server_chan
            ))?;
        }
    }

    Ok(())
}

/// find_capability_source will walk the component tree to find the originating source of a
/// capability, starting on the given abs_moniker. It returns the absolute moniker of the
/// originating component, a reference to its realm, and the capability exposed or offered at the
/// originating source. If the absolute moniker and realm are None, then the capability originates
/// at the returned path in componentmgr's namespace.
async fn find_capability_source<'a>(
    model: &'a Model,
    used_capability: &'a Capability,
    abs_moniker: AbsoluteMoniker,
) -> Result<CapabilitySource, ModelError> {
    // Holds mutable state as we walk the tree
    struct State {
        // The capability as it's represented in the current component
        capability: Capability,
        // The name of the child we came from
        name: Option<ChildMoniker>,
        // The moniker of the component we are currently looking at
        moniker: AbsoluteMoniker,
    }
    let moniker = match abs_moniker.parent() {
        Some(m) => m,
        None => return Ok(CapabilitySource::ComponentMgrNamespace(used_capability.clone())),
    };
    let mut s = State {
        capability: used_capability.clone(),
        name: abs_moniker.path().last().map(|c| c.clone()),
        moniker: moniker,
    };
    // Walk offer chain
    'offerloop: loop {
        let current_realm_mutex = await!(model.look_up_realm(&s.moniker))?;
        let current_realm = await!(current_realm_mutex.lock());
        // This unwrap is safe because look_up_realm populates this field
        let decl = current_realm.instance.decl.as_ref().expect("missing offer decl");

        if let Some(offer) = decl.find_offer_source(&s.capability, &s.name.unwrap().name()) {
            match &offer.source {
                OfferSource::Realm => {
                    // The offered capability comes from the realm, so follow the
                    // parent
                    s.capability = offer.capability.clone();
                    s.name = s.moniker.path().last().map(|c| c.clone());
                    s.moniker = match s.moniker.parent() {
                        Some(m) => m,
                        None => {
                            return Ok(CapabilitySource::ComponentMgrNamespace(
                                s.capability.clone(),
                            ))
                        }
                    };
                    continue 'offerloop;
                }
                OfferSource::Myself => {
                    // The offered capability comes from the current component,
                    // return our current location in the tree.
                    return Ok(CapabilitySource::Component(
                        offer.capability.clone(),
                        current_realm_mutex.clone(),
                    ));
                }
                OfferSource::Child(child_name) => {
                    // The offered capability comes from a child, break the loop
                    // and begin walking the expose chain.
                    s.capability = offer.capability.clone();
                    s.moniker = s.moniker.child(ChildMoniker::new(child_name.to_string()));
                    break 'offerloop;
                }
            }
        } else {
            return Err(ModelError::capability_discovery_error(format_err!(
                "no matching offers found for capability {} from component {}",
                s.capability,
                s.moniker
            )));
        }
    }
    // Walk expose chain
    loop {
        let current_realm_mutex = await!(model.look_up_realm(&s.moniker))?;
        let current_realm = await!(current_realm_mutex.lock());
        // This unwrap is safe because look_up_realm populates this field
        let decl = current_realm.instance.decl.as_ref().expect("missing expose decl");

        if let Some(expose) = decl.find_expose_source(&s.capability) {
            match &expose.source {
                ExposeSource::Myself => {
                    // The offered capability comes from the current component, return our
                    // current location in the tree.
                    return Ok(CapabilitySource::Component(
                        expose.capability.clone(),
                        current_realm_mutex.clone(),
                    ));
                }
                ExposeSource::Child(child_name) => {
                    // The offered capability comes from a child, so follow the child.
                    s.capability = expose.capability.clone();
                    s.moniker = s.moniker.child(ChildMoniker::new(child_name.to_string()));
                    continue;
                }
            }
        } else {
            // We didn't find any matching exposes! Oh no!
            return Err(ModelError::capability_discovery_error(format_err!(
                "no matching exposes found for capability {} from component {}",
                s.capability,
                s.moniker
            )));
        }
    }
}
