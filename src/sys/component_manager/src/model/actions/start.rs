// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::model::{
        error::ModelError,
        exposed_dir::ExposedDir,
        hooks::{Event, EventPayload, RuntimeInfo},
        moniker::AbsoluteMoniker,
        namespace::IncomingNamespace,
        realm::{ExecutionState, Realm, Runtime, WeakRealm},
        routing_facade::RoutingFacade,
    },
    cm_rust::data,
    fidl::endpoints::{self, Proxy, ServerEnd},
    fidl_fuchsia_component_runner as fcrunner,
    fidl_fuchsia_io::DirectoryProxy,
    fidl_fuchsia_sys2 as fsys,
    fuchsia_async::{self as fasync, EHandle},
    fuchsia_zircon as zx,
    log::*,
    std::sync::Arc,
    vfs::execution_scope::ExecutionScope,
};

pub(super) async fn do_start(realm: Arc<Realm>) -> Result<(), ModelError> {
    // Pre-flight check: if the component is already started, return now. Note that `bind_at` also
    // performs this check before scheduling the action; here, we do it again while the action is
    // registered so we avoid the risk of invoking the BeforeStart hook twice.
    {
        let execution = realm.lock_execution().await;
        if let Some(res) = should_return_early(&execution, &realm.abs_moniker) {
            return res;
        }
    }

    // Resolve the component.
    let component = realm.resolve().await?;

    // Find the runner to use.
    let runner = realm.resolve_runner().await.map_err(|e| {
        error!("failed to resolve runner for {}: {:?}", realm.abs_moniker, e);
        e
    })?;

    // Generate the Runtime which will be set in the Execution.
    let (pending_runtime, start_info, controller_server) = make_execution_runtime(
        realm.as_weak(),
        component.resolved_url.clone(),
        component.package,
        &component.decl,
    )
    .await?;

    // Invoke the BeforeStart hook.
    {
        let routing_facade = RoutingFacade::new();
        let event = Event::new(
            realm.abs_moniker.clone(),
            Ok(EventPayload::Started {
                component_url: realm.component_url.clone(),
                runtime: RuntimeInfo::from_runtime(&pending_runtime),
                component_decl: component.decl.clone(),
                routing_facade,
            }),
        );
        realm.hooks.dispatch(&event).await?;
    }

    // Set the Runtime in the Execution. From component manager's perspective, this indicates
    // that the component has started. This may return early if the component is shut down.
    {
        let mut execution = realm.lock_execution().await;
        if let Some(res) = should_return_early(&execution, &realm.abs_moniker) {
            return res;
        }
        execution.runtime = Some(pending_runtime);
    }

    // It's possible that the component is stopped before getting here. If so, that's fine: the
    // runner will start the component, but its stop or kill signal will be immediately set on the
    // component controller.
    runner.start(start_info, controller_server).await;

    Ok(())
}

/// Returns `Some(Result)` if `bind` should return early based on either of the following:
/// - The component instance is shut down.
/// - The component instance is already started.
pub fn should_return_early(
    execution: &ExecutionState,
    abs_moniker: &AbsoluteMoniker,
) -> Option<Result<(), ModelError>> {
    if execution.is_shut_down() {
        Some(Err(ModelError::instance_shut_down(abs_moniker.clone())))
    } else if execution.runtime.is_some() {
        Some(Ok(()))
    } else {
        None
    }
}

/// Returns a configured Runtime for a component and the start info (without actually starting
/// the component).
async fn make_execution_runtime(
    realm: WeakRealm,
    url: String,
    package: Option<fsys::Package>,
    decl: &cm_rust::ComponentDecl,
) -> Result<
    (Runtime, fcrunner::ComponentStartInfo, ServerEnd<fcrunner::ComponentControllerMarker>),
    ModelError,
> {
    // Create incoming/outgoing directories, and populate them.
    let exposed_dir = ExposedDir::new(
        ExecutionScope::from_executor(Box::new(EHandle::local())),
        realm.clone(),
        decl.clone(),
    )?;
    let (outgoing_dir_client, outgoing_dir_server) =
        zx::Channel::create().map_err(|e| ModelError::namespace_creation_failed(e))?;
    let (runtime_dir_client, runtime_dir_server) =
        zx::Channel::create().map_err(|e| ModelError::namespace_creation_failed(e))?;
    let mut namespace = IncomingNamespace::new(package)?;
    let ns = namespace.populate(realm, decl).await?;

    let (controller_client, controller_server) =
        endpoints::create_endpoints::<fcrunner::ComponentControllerMarker>()
            .expect("could not create component controller endpoints");
    let controller =
        controller_client.into_proxy().expect("failed to create ComponentControllerProxy");
    // Set up channels into/out of the new component.
    let runtime = Runtime::start_from(
        url.clone(),
        Some(namespace),
        Some(DirectoryProxy::from_channel(
            fasync::Channel::from_channel(outgoing_dir_client).unwrap(),
        )),
        Some(DirectoryProxy::from_channel(
            fasync::Channel::from_channel(runtime_dir_client).unwrap(),
        )),
        exposed_dir,
        Some(controller),
    )?;
    let start_info = fcrunner::ComponentStartInfo {
        resolved_url: Some(url),
        program: data::clone_option_dictionary(&decl.program),
        ns: Some(ns),
        outgoing_dir: Some(ServerEnd::new(outgoing_dir_server)),
        runtime_dir: Some(ServerEnd::new(runtime_dir_server)),
    };

    Ok((runtime, start_info, controller_server))
}
