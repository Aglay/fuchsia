// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::{format_err, Error};
use fidl::endpoints::{DiscoverableService, Proxy, ServiceMarker};
use futures::future::BoxFuture;

use fuchsia_async as fasync;
use fuchsia_component::client::connect_to_service;

use fuchsia_zircon as zx;
use futures::lock::Mutex;
use std::sync::Arc;

pub type GenerateService =
    Box<dyn Fn(&str, zx::Channel) -> BoxFuture<'static, Result<(), Error>> + Send + Sync>;

pub type ServiceContextHandle = Arc<Mutex<ServiceContext>>;

/// A wrapper around service operations, allowing redirection to a nested
/// environment.
pub struct ServiceContext {
    generate_service: Option<GenerateService>,
}

impl ServiceContext {
    pub fn create(generate_service: Option<GenerateService>) -> ServiceContextHandle {
        return Arc::new(Mutex::new(ServiceContext::new(generate_service)));
    }

    pub fn new(generate_service: Option<GenerateService>) -> Self {
        Self { generate_service: generate_service }
    }

    pub async fn connect<S: DiscoverableService>(&self) -> Result<S::Proxy, Error> {
        if let Some(generate_service) = &self.generate_service {
            let (client, server) = zx::Channel::create()?;
            ((generate_service)(S::SERVICE_NAME, server)).await?;
            return Ok(S::Proxy::from_channel(fasync::Channel::from_channel(client)?));
        } else {
            return connect_to_service::<S>();
        }
    }

    pub async fn connect_named<S: ServiceMarker>(
        &self,
        service_name: &str,
    ) -> Result<S::Proxy, Error> {
        if let Some(generate_service) = &self.generate_service {
            let (client, server) = zx::Channel::create()?;
            if (generate_service)(service_name, server).await.is_err() {
                return Err(format_err!("Could not handl service {:?}", service_name));
            }

            Ok(S::Proxy::from_channel(fasync::Channel::from_channel(client)?))
        } else {
            Err(format_err!("No service generator"))
        }
    }
}
