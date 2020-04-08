// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        capability::CapabilitySource,
        model::{
            error::ModelError,
            hooks::{Event, EventPayload, EventType, Hook, HooksRegistration},
            moniker::AbsoluteMoniker,
        },
    },
    async_trait::async_trait,
    log::info,
    std::sync::{Arc, Weak},
};

pub struct CapabilityRoutedLogger;

impl CapabilityRoutedLogger {
    pub fn new() -> Self {
        Self
    }

    pub fn hooks(self: &Arc<Self>) -> Vec<HooksRegistration> {
        vec![HooksRegistration::new(
            "CapabilityRouterLogger",
            vec![EventType::CapabilityRouted],
            Arc::downgrade(self) as Weak<dyn Hook>,
        )]
    }

    async fn on_capability_routed_async(
        self: Arc<Self>,
        target_moniker: &AbsoluteMoniker,
        source: &CapabilitySource,
    ) -> Result<(), ModelError> {
        match source {
            CapabilitySource::Component { capability, realm } => {
                info!(
                    "'{}' routed from '{}' to '{}'",
                    capability.source_id(),
                    realm.moniker,
                    target_moniker
                );
            }
            CapabilitySource::Framework { capability, .. } => {
                info!("'{}' routed from framework to '{}'", capability.source_id(), target_moniker);
            }
            _ => {}
        }
        Ok(())
    }
}

#[async_trait]
impl Hook for CapabilityRoutedLogger {
    async fn on(self: Arc<Self>, event: &Event) -> Result<(), ModelError> {
        match &event.payload {
            EventPayload::CapabilityRouted { source, .. } => {
                self.on_capability_routed_async(&event.target_moniker, &source).await?;
            }
            _ => {}
        }
        Ok(())
    }
}
