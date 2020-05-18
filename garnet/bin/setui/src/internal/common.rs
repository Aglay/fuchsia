// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use chrono::{DateTime, Utc};

#[derive(PartialEq, Clone, Debug, Eq, Hash)]
pub enum Address {}

/// Macro for defining a message hub with no addressable messengers
#[macro_export]
macro_rules! anonymous_message_hub_definition {
    ($payload:ty) => {
        crate::message_hub_definition!($payload, crate::internal::common::Address);
    };
}

/// Macro for defining a standard message hub
#[macro_export]
macro_rules! message_hub_definition {
    ($payload:ty, $address:ty) => {
        pub mod message {
            use crate::message::base::Signature as BaseSignature;
            use crate::message::message_client::MessageClient as BaseMessageClient;
            use crate::message::message_hub::MessageHub;
            use crate::message::messenger::{
                MessengerClient as BaseMessengerClient, MessengerFactory as BaseFactory,
            };
            use crate::message::receptor::Receptor as BaseReceptor;

            pub type Factory = BaseFactory<$payload, $address>;
            pub type Messenger = BaseMessengerClient<$payload, $address>;
            pub type Client = BaseMessageClient<$payload, $address>;

            #[allow(dead_code)]
            pub type Receptor = BaseReceptor<$payload, $address>;

            #[allow(dead_code)]
            pub type Signature = BaseSignature<$address>;

            pub fn create_hub() -> Factory {
                MessageHub::<$payload, $address>::create()
            }
        }
    };
}

/// Representation of time used for logging.
pub type Timestamp = DateTime<Utc>;

pub fn now() -> Timestamp {
    Utc::now()
}
