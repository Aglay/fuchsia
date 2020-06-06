// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
use crate::internal::agent::message::Receptor;
use crate::internal::event;
use crate::service_context::ServiceContextHandle;
use crate::switchboard::base::{SettingType, SwitchboardClient};
use anyhow::Error;
use async_trait::async_trait;
use core::fmt::Debug;
use futures::channel::mpsc::UnboundedSender;
use futures::future::BoxFuture;
use std::collections::HashSet;
use std::sync::Arc;
use thiserror::Error;

pub type AgentId = usize;

pub type GenerateAgent = Arc<dyn Fn(Context) + Send + Sync>;

pub type InvocationResult = Result<(), AgentError>;
pub type InvocationSender = UnboundedSender<Invocation>;

#[derive(Error, Debug, Clone)]
pub enum AgentError {
    #[error("Unhandled Lifespan")]
    UnhandledLifespan,
    #[error("Unexpected Error")]
    UnexpectedError,
}

/// Identification for the agent used for logging purposes.
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub enum Descriptor {
    Component(&'static str),
}

/// TODO(b/52428): Move lifecycle stage context contents here.
pub struct Context {
    pub receptor: Receptor,
    publisher: event::Publisher,
}

impl Context {
    pub async fn new(
        receptor: Receptor,
        descriptor: Descriptor,
        event_factory: event::message::Factory,
    ) -> Self {
        Self {
            receptor,
            publisher: event::Publisher::create(&event_factory, event::Address::Agent(descriptor))
                .await,
        }
    }

    pub fn get_publisher(&self) -> event::Publisher {
        self.publisher.clone()
    }
}

/// The scope of an agent's life. Initialization components should
/// only run at the beginning of the service. Service components follow
/// initialization and run for the duration of the service.
#[derive(Clone, Debug)]
pub enum Lifespan {
    Initialization(InitializationContext),
    Service(RunContext),
}

#[derive(Clone, Debug)]
pub struct InitializationContext {
    pub available_components: HashSet<SettingType>,
    pub switchboard_client: SwitchboardClient,
}

impl InitializationContext {
    pub fn new(switchboard_client: SwitchboardClient, components: HashSet<SettingType>) -> Self {
        Self { available_components: components, switchboard_client: switchboard_client }
    }
}

#[derive(Clone, Debug)]
pub struct RunContext {
    pub switchboard_client: SwitchboardClient,
}

/// Struct of information passed to the agent during each invocation.
#[derive(Clone, Debug)]
pub struct Invocation {
    pub lifespan: Lifespan,
    pub service_context: ServiceContextHandle,
}

/// Blueprint defines an interface provided to the authority for constructing
/// a given agent.
pub trait Blueprint {
    /// Returns the Agent descriptor to be associated with components used
    /// by this agent, such as logging.
    fn get_descriptor(&self) -> Descriptor;

    /// Uses the supplied context to create agent.
    fn create(&self, context: Context) -> BoxFuture<'static, ()>;
}

pub type BlueprintHandle = Arc<dyn Blueprint + Send + Sync>;

/// Entity for registering agents. It is responsible for signaling
/// Stages based on the specified lifespan.
#[async_trait]
pub trait Authority {
    async fn register(&mut self, blueprint: BlueprintHandle) -> Result<(), Error>;
}

#[macro_export]
macro_rules! blueprint_definition {
    ($descriptor:stmt, $create:expr) => {
        pub mod blueprint {
            #[allow(unused_imports)]
            use super::*;
            use crate::agent::base;
            use futures::future::BoxFuture;
            use std::sync::Arc;

            pub fn create() -> base::BlueprintHandle {
                Arc::new(BlueprintImpl)
            }

            struct BlueprintImpl;

            impl base::Blueprint for BlueprintImpl {
                fn get_descriptor(&self) -> base::Descriptor {
                    $descriptor
                }

                fn create(&self, context: base::Context) -> BoxFuture<'static, ()> {
                    Box::pin(async move {
                        $create(context).await;
                    })
                }
            }
        }
    };
}
