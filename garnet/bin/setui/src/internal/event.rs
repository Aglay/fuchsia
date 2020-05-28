// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::agent;
use crate::message::base::{Audience, MessengerType};
use crate::message_hub_definition;
use std::sync::Arc;

#[derive(Clone, Debug)]
pub enum Payload {
    Event(Event),
}

/// Top level definition of events in the setting service. A new type should
/// be defined for each component (setting, agent, switchboard, registry, etc.)
#[derive(Clone, Debug, PartialEq)]
pub enum Event {
    Custom(&'static str),
}

#[derive(PartialEq, Clone, Debug, Eq, Hash)]
pub enum Address {
    Agent(agent::base::Descriptor),
}

// The Event message hub should be used to capture events not normally exposed
// through other communication. For example, actions that happen within agents
// are generally not reported back. Events that are useful for diagnostics and
// verification in tests should be defined here.
message_hub_definition!(crate::internal::event::Payload, crate::internal::event::Address);

/// Publisher is a helper for producing logs. It simplifies message creation for
/// each event and associates an address with these messages at construction.
#[derive(Clone)]
pub struct Publisher {
    messenger: message::Messenger,
}

impl Publisher {
    pub async fn create(factory: &message::Factory, address: Address) -> Publisher {
        let (messenger, _) = factory
            .create(MessengerType::Addressable(address))
            .await
            .expect("should be able to retrieve messenger for publisher");

        Publisher { messenger: messenger }
    }

    /// Broadcasts event to the message hub.
    pub fn send_event(&self, event: Event) {
        self.messenger.message(Payload::Event(event), Audience::Broadcast).send().ack();
    }
}

pub mod subscriber {
    use super::*;
    use futures::future::BoxFuture;

    pub type BlueprintHandle = Arc<dyn Blueprint>;

    /// The Subscriber Blueprint is used for spawning new subscribers on demand
    /// in the environment.
    pub trait Blueprint {
        fn create(&self, message_factory: message::Factory) -> BoxFuture<'static, ()>;
    }
}

/// This macro helps define a blueprint for a given async subscriber creation
/// method.
#[macro_export]
macro_rules! subscriber_blueprint {
    ($create:expr) => {
        pub mod event {
            #[allow(unused_imports)]
            use super::*;
            use crate::internal::event;
            use futures::future::BoxFuture;
            use std::sync::Arc;

            pub fn create() -> event::subscriber::BlueprintHandle {
                Arc::new(BlueprintImpl)
            }

            struct BlueprintImpl;

            impl event::subscriber::Blueprint for BlueprintImpl {
                fn create(
                    &self,
                    message_factory: event::message::Factory,
                ) -> BoxFuture<'static, ()> {
                    Box::pin(async move {
                        $create(context).await;
                    })
                }
            }
        }
    };
}
