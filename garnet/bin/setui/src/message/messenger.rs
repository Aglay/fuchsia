// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::message::base::{
    ActionSender, Address, Audience, CreateMessengerResult, Message, MessageAction, MessageError,
    MessageType, MessengerAction, MessengerActionSender, MessengerId, MessengerType, Payload,
};
use crate::message::beacon::Beacon;
use crate::message::message_builder::MessageBuilder;

/// MessengerFactory is the artifact of creating a MessageHub. It can be used
/// to create new messengers.
#[derive(Clone)]
pub struct MessengerFactory<P: Payload + 'static, A: Address + 'static> {
    messenger_action_tx: MessengerActionSender<P, A>,
}

impl<P: Payload + 'static, A: Address + 'static> MessengerFactory<P, A> {
    pub(super) fn new(action_tx: MessengerActionSender<P, A>) -> MessengerFactory<P, A> {
        MessengerFactory { messenger_action_tx: action_tx }
    }

    pub async fn create(&self, messenger_type: MessengerType<A>) -> CreateMessengerResult<P, A> {
        let (tx, rx) = futures::channel::oneshot::channel::<CreateMessengerResult<P, A>>();

        self.messenger_action_tx.unbounded_send(MessengerAction::Create(messenger_type, tx)).ok();

        if let Ok(result) = rx.await {
            return result;
        } else {
            return Err(MessageError::Unexpected);
        }
    }
}

/// Messengers provide clients the ability to send messages to other registered
/// clients. They can only be created through a MessageHub.
#[derive(Clone)]
pub struct Messenger<P: Payload + 'static, A: Address + 'static> {
    id: MessengerId,
    action_tx: ActionSender<P, A>,
}

impl<P: Payload + 'static, A: Address + 'static> Messenger<P, A> {
    pub(super) fn create(id: MessengerId, action_tx: ActionSender<P, A>) -> Messenger<P, A> {
        Messenger { id: id, action_tx: action_tx }
    }

    /// Creates a MessageBuilder for a new message with the specified payload
    /// and audience.
    pub fn message(&self, payload: P, audience: Audience<A>) -> MessageBuilder<P, A> {
        MessageBuilder::new(payload, MessageType::Origin(audience), self.clone())
    }

    /// Returns the identification for this Messenger.
    pub(super) fn get_id(&self) -> MessengerId {
        self.id
    }

    /// Forwards the message to the next Messenger. Note that this method is
    /// private and only called through the MessageClient.
    pub(super) fn forward(&self, message: Message<P, A>, beacon: Option<Beacon<P, A>>) {
        self.transmit(MessageAction::Forward(message), beacon);
    }

    /// Tranmits a given action to the message hub. This is a common utility
    /// method to be used for immediate actions (forwarding, observing) and
    /// deferred actions as well (sending, replying).
    pub(super) fn transmit(&self, action: MessageAction<P, A>, beacon: Option<Beacon<P, A>>) {
        self.action_tx.unbounded_send((self.id, action, beacon)).ok();
    }
}
