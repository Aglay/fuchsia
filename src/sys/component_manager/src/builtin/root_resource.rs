// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        builtin::capability::{BuiltinCapability, BuiltinCapabilityProvider},
        capability::*,
        model::{
            error::ModelError,
            hooks::{Event, EventPayload, Hook},
        },
    },
    anyhow::Error,
    async_trait::async_trait,
    cm_rust::CapabilityPath,
    fidl_fuchsia_boot as fboot,
    fuchsia_zircon::{self as zx, HandleBased, Resource},
    futures::prelude::*,
    lazy_static::lazy_static,
    std::{convert::TryInto, sync::Arc},
};

lazy_static! {
    static ref ROOT_RESOURCE_CAPABILITY_PATH: CapabilityPath =
        "/svc/fuchsia.boot.RootResource".try_into().unwrap();
}

/// An implementation of the `fuchsia.boot.RootResource` protocol.
pub struct RootResource {
    resource: Resource,
}

impl RootResource {
    pub fn new(resource: Resource) -> Arc<Self> {
        Arc::new(Self { resource })
    }
}

#[async_trait]
impl BuiltinCapability for RootResource {
    const NAME: &'static str = "RootResource";
    type Marker = fboot::RootResourceMarker;

    async fn serve(
        self: Arc<Self>,
        mut stream: fboot::RootResourceRequestStream,
    ) -> Result<(), Error> {
        while let Some(fboot::RootResourceRequest::Get { responder }) = stream.try_next().await? {
            responder.send(self.resource.duplicate_handle(zx::Rights::SAME_RIGHTS)?)?;
        }
        Ok(())
    }

    async fn on_framework_capability_routed<'a>(
        self: &'a Arc<Self>,
        capability: &'a InternalCapability,
        capability_provider: Option<Box<dyn CapabilityProvider>>,
    ) -> Result<Option<Box<dyn CapabilityProvider>>, ModelError> {
        match capability {
            InternalCapability::Protocol(capability_path)
                if *capability_path == *ROOT_RESOURCE_CAPABILITY_PATH =>
            {
                Ok(Some(Box::new(BuiltinCapabilityProvider::<RootResource>::new(Arc::downgrade(
                    &self,
                )))))
            }
            _ => Ok(capability_provider),
        }
    }
}

#[async_trait]
impl Hook for RootResource {
    async fn on(self: Arc<Self>, event: &Event) -> Result<(), ModelError> {
        if let Ok(EventPayload::CapabilityRouted {
            source: CapabilitySource::AboveRoot { capability },
            capability_provider,
        }) = &event.result
        {
            let mut provider = capability_provider.lock().await;
            *provider = self.on_framework_capability_routed(&capability, provider.take()).await?;
        };
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        crate::model::{hooks::Hooks, moniker::AbsoluteMoniker},
        fidl::endpoints::ClientEnd,
        fuchsia_async as fasync,
        futures::lock::Mutex,
        std::path::PathBuf,
    };

    #[fasync::run_singlethreaded(test)]
    async fn can_connect() -> Result<(), Error> {
        let root_resource = RootResource::new(Resource::from(zx::Handle::invalid()));
        let hooks = Hooks::new(None);
        hooks.install(root_resource.hooks()).await;

        let provider = Arc::new(Mutex::new(None));
        let source = CapabilitySource::AboveRoot {
            capability: InternalCapability::Protocol(ROOT_RESOURCE_CAPABILITY_PATH.clone()),
        };

        let event = Event::new_for_test(
            AbsoluteMoniker::root(),
            "fuchsia-pkg://root",
            Ok(EventPayload::CapabilityRouted { source, capability_provider: provider.clone() }),
        );
        hooks.dispatch(&event).await?;

        let (client, mut server) = zx::Channel::create()?;
        if let Some(provider) = provider.lock().await.take() {
            provider.open(0, 0, PathBuf::new(), &mut server).await?;
        }

        // We do not call get, as we passed an invalid handle to RootResource,
        // which would cause a PEER_CLOSED failure. We passed an invalid handle
        // to RootResource because you need a Resource to create another one,
        // which we do not have.
        ClientEnd::<fboot::RootJobMarker>::new(client)
            .into_proxy()
            .expect("Failed to create proxy");
        Ok(())
    }
}
