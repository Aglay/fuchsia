// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        capability::{CapabilityProvider, CapabilitySource, FrameworkCapability},
        model::{
            actions::Action,
            error::ModelError,
            hooks::{Event, EventPayload, EventType, Hook, HooksRegistration},
            model::Model,
            realm::Realm,
        },
    },
    anyhow::{Context as _, Error},
    async_trait::async_trait,
    cm_rust::CapabilityPath,
    fidl::endpoints::ServerEnd,
    fidl_fuchsia_sys2::*,
    fuchsia_async::{self as fasync},
    fuchsia_zircon as zx,
    futures::{future::BoxFuture, prelude::*},
    lazy_static::lazy_static,
    log::warn,
    std::{
        convert::TryInto,
        sync::{Arc, Weak},
    },
};

lazy_static! {
    pub static ref SYSTEM_CONTROLLER_CAPABILITY_PATH: CapabilityPath =
        "/svc/fuchsia.sys2.SystemController".try_into().unwrap();
}

#[derive(Clone)]
pub struct SystemController {
    inner: Arc<SystemControllerInner>,
}

impl SystemController {
    pub fn new(model: Arc<Model>) -> Self {
        Self { inner: Arc::new(SystemControllerInner::new(model)) }
    }

    pub fn hooks(&self) -> Vec<HooksRegistration> {
        vec![HooksRegistration {
            events: vec![EventType::RouteCapability],
            callback: Arc::downgrade(&self.inner) as Weak<dyn Hook>,
        }]
    }
}

struct SystemControllerInner {
    model: Arc<Model>,
}

impl SystemControllerInner {
    pub fn new(model: Arc<Model>) -> Self {
        Self { model }
    }

    async fn on_route_framework_capability_async<'a>(
        self: Arc<Self>,
        capability: &'a FrameworkCapability,
        capability_provider: Option<Box<dyn CapabilityProvider>>,
    ) -> Result<Option<Box<dyn CapabilityProvider>>, ModelError> {
        match capability {
            FrameworkCapability::ServiceProtocol(capability_path)
                if *capability_path == *SYSTEM_CONTROLLER_CAPABILITY_PATH =>
            {
                Ok(Some(Box::new(SystemControllerCapabilityProvider::new(self.model.clone()))
                    as Box<dyn CapabilityProvider>))
            }
            _ => Ok(capability_provider),
        }
    }
}

impl Hook for SystemControllerInner {
    fn on(self: Arc<Self>, event: &Event) -> BoxFuture<Result<(), ModelError>> {
        Box::pin(async move {
            if let EventPayload::RouteCapability {
                source: CapabilitySource::Framework { capability, scope_moniker: None },
                capability_provider,
            } = &event.payload
            {
                let mut capability_provider = capability_provider.lock().await;
                *capability_provider = self
                    .on_route_framework_capability_async(&capability, capability_provider.take())
                    .await?;
            };
            Ok(())
        })
    }
}

pub struct SystemControllerCapabilityProvider {
    model: Arc<Model>,
}

impl SystemControllerCapabilityProvider {
    pub fn new(model: Arc<Model>) -> Self {
        Self { model }
    }

    async fn open_async(self, mut stream: SystemControllerRequestStream) -> Result<(), Error> {
        while let Some(request) = stream.try_next().await? {
            // TODO(jmatt) There is the potential for a race here. If
            // the thing that called SystemController.Shutdown is a
            // component that component_manager controls, it should
            // be gone by now. Sending a response doesn't make a lot
            // of sense in this case. However, the caller might live
            // outside component_manager, in which case a response
            // does make sense. Figure out if our behavior should be
            // different and/or whether we should drop the response
            // from this API.
            match request {
                // Shutting down the root component causes component_manager to
                // exit. main.rs waits on the model to observe the root realm
                // disappear.
                SystemControllerRequest::Shutdown { responder } => {
                    Realm::register_action(
                        self.model.root_realm.clone(),
                        self.model.clone(),
                        Action::Shutdown,
                    )
                    .await
                    .await
                    .context("got error waiting for shutdown action to complete")?;

                    match responder.send() {
                        Ok(()) => {}
                        Err(e) => {
                            println!(
                                "error sending response to shutdown requester:\
                                 {}\n shut down proceeding",
                                e
                            );
                        }
                    }
                }
            }
        }
        Ok(())
    }
}

#[async_trait]
impl CapabilityProvider for SystemControllerCapabilityProvider {
    async fn open(
        self: Box<Self>,
        _flags: u32,
        _open_mode: u32,
        _relative_path: String,
        server_end: zx::Channel,
    ) -> Result<(), ModelError> {
        let server_end = ServerEnd::<SystemControllerMarker>::new(server_end);
        let stream: SystemControllerRequestStream = server_end.into_stream().unwrap();
        fasync::spawn(async move {
            let result = self.open_async(stream).await;
            if let Err(e) = result {
                warn!("SystemController.open failed: {}", e);
            }
        });

        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use {
        crate::capability::CapabilityProvider,
        crate::model::{
            binding::Binder,
            testing::test_helpers::{
                component_decl_with_test_runner, ActionsTest, ComponentDeclBuilder, ComponentInfo,
                TEST_RUNNER_NAME,
            },
        },
        crate::system_controller::SystemControllerCapabilityProvider,
        fidl::endpoints,
        fidl_fuchsia_sys2 as fsys,
    };

    /// Use SystemController to shut down a system whose root has the child `a`
    /// and `a` has descendents as shown in the diagram below.
    ///  a
    ///   \
    ///    b
    ///   / \
    ///  c   d
    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_system_controller() {
        // Configure and start realm
        let components = vec![
            (
                "root",
                ComponentDeclBuilder::new()
                    .add_lazy_child("a")
                    .offer_runner_to_children(TEST_RUNNER_NAME)
                    .build(),
            ),
            (
                "a",
                ComponentDeclBuilder::new()
                    .add_eager_child("b")
                    .offer_runner_to_children(TEST_RUNNER_NAME)
                    .build(),
            ),
            (
                "b",
                ComponentDeclBuilder::new()
                    .add_eager_child("c")
                    .add_eager_child("d")
                    .offer_runner_to_children(TEST_RUNNER_NAME)
                    .build(),
            ),
            ("c", component_decl_with_test_runner()),
            ("d", component_decl_with_test_runner()),
        ];
        let test = ActionsTest::new("root", components, None).await;
        let realm_a = test.look_up(vec!["a:0"].into()).await;
        let realm_b = test.look_up(vec!["a:0", "b:0"].into()).await;
        let realm_c = test.look_up(vec!["a:0", "b:0", "c:0"].into()).await;
        let realm_d = test.look_up(vec!["a:0", "b:0", "d:0"].into()).await;
        test.model.bind(&realm_a.abs_moniker).await.expect("could not bind to a");

        // Wire up connections to SystemController
        let sys_controller = Box::new(SystemControllerCapabilityProvider::new(test.model.clone()));
        let (client_channel, server_channel) =
            endpoints::create_endpoints::<fsys::SystemControllerMarker>()
                .expect("failed creating channel endpoints");
        sys_controller
            .open(0, 0, "".to_string(), server_channel.into_channel())
            .await
            .expect("failed to open capability");
        let controller_proxy =
            client_channel.into_proxy().expect("failed converting endpoint into proxy");

        let root_realm_info = ComponentInfo::new(test.model.root_realm.clone()).await;
        let realm_a_info = ComponentInfo::new(realm_a.clone()).await;
        let realm_b_info = ComponentInfo::new(realm_b.clone()).await;
        let realm_c_info = ComponentInfo::new(realm_c.clone()).await;
        let realm_d_info = ComponentInfo::new(realm_d.clone()).await;

        // Check that the root realm is still here
        root_realm_info.check_not_shut_down(&test.runner).await;
        realm_a_info.check_not_shut_down(&test.runner).await;
        realm_b_info.check_not_shut_down(&test.runner).await;
        realm_c_info.check_not_shut_down(&test.runner).await;
        realm_d_info.check_not_shut_down(&test.runner).await;

        // Ask the SystemController to shut down the system and wait to be
        // notified that the room realm stopped.
        let completion = test.builtin_environment.wait_for_root_realm_stop();
        controller_proxy.shutdown().await.expect("shutdown request failed");
        completion.await;

        // Check state bits to confirm root realm looks shut down
        root_realm_info.check_is_shut_down(&test.runner).await;
        realm_a_info.check_is_shut_down(&test.runner).await;
        realm_b_info.check_is_shut_down(&test.runner).await;
        realm_c_info.check_is_shut_down(&test.runner).await;
        realm_d_info.check_is_shut_down(&test.runner).await;
    }
}
