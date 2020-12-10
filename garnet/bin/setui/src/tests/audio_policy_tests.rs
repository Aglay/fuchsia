// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::audio::policy::{
    PolicyId, Property, PropertyTarget, Request as AudioRequest, Response as AudioResponse,
    StateBuilder, Transform, TransformFlags,
};
use crate::internal;
use crate::message::base::{Audience, MessengerType};
use crate::policy::base::{response::Payload, Request};
use crate::switchboard::base::{AudioStreamType, SettingType};
use std::collections::{HashMap, HashSet};
use std::iter::FromIterator;

// Verifies that the audio policy state builder functions correctly for adding targets and
// transforms.
#[fuchsia_async::run_until_stalled(test)]
async fn test_state_builder() {
    let properties: HashMap<AudioStreamType, TransformFlags> = [
        (AudioStreamType::Background, TransformFlags::TRANSFORM_MAX),
        (AudioStreamType::Media, TransformFlags::TRANSFORM_MIN),
    ]
    .iter()
    .cloned()
    .collect();
    let mut builder = StateBuilder::new();

    for (property, value) in &properties {
        builder = builder.add_property(property.clone(), value.clone());
    }

    let state = builder.build();
    let retrieved_properties = state.get_properties();
    assert_eq!(retrieved_properties.len(), properties.len());

    let mut seen_targets = HashSet::<PropertyTarget>::new();
    for property in retrieved_properties.iter().cloned() {
        let target = property.target;
        // Make sure only unique targets are encountered.
        assert!(!seen_targets.contains(&target));
        seen_targets.insert(target);
        // Ensure the specified transforms are present.
        assert_eq!(
            property.available_transforms,
            *properties.get(&property.stream_type).expect("should be here")
        );
    }
}

// Verifies that adding transforms to policy properties works.
#[fuchsia_async::run_until_stalled(test)]
async fn test_property_transforms() {
    let supported_transforms = TransformFlags::TRANSFORM_MAX | TransformFlags::TRANSFORM_MIN;
    let transforms = [Transform::Min(0.1), Transform::Max(0.9)];
    let mut property = Property::new(AudioStreamType::Media, supported_transforms);
    let mut property2 = Property::new(AudioStreamType::Background, supported_transforms);

    for transform in transforms.iter().cloned() {
        property.add_transform(transform);
        property2.add_transform(transform);
    }

    // Ensure policy size matches transforms specified.
    assert_eq!(property.active_policies.len(), transforms.len());
    assert_eq!(property2.active_policies.len(), transforms.len());

    let mut retrieved_ids: HashSet<PolicyId> =
        HashSet::from_iter(property.active_policies.iter().map(|policy| policy.id));
    retrieved_ids.extend(property2.active_policies.iter().map(|policy| policy.id));

    // Make sure all ids are unique, even across properties.
    assert_eq!(retrieved_ids.len(), transforms.len() * 2);

    // Verify transforms are present.
    let retrieved_transforms: Vec<Transform> =
        property.active_policies.iter().map(|policy| policy.transform).collect();
    let retrieved_transforms2: Vec<Transform> =
        property2.active_policies.iter().map(|policy| policy.transform).collect();
    for transform in transforms.iter() {
        assert!(retrieved_transforms.contains(&transform));
        assert!(retrieved_transforms2.contains(&transform));
    }
}

// A simple validation test to ensure the policy message hub propagates messages
// properly.
#[fuchsia_async::run_until_stalled(test)]
async fn test_policy_message_hub() {
    let messenger_factory = internal::policy::message::create_hub();
    let policy_handler_address = internal::policy::Address::Policy(SettingType::Audio);

    // Create messenger to send request.
    let (messenger, receptor) = messenger_factory
        .create(MessengerType::Unbound)
        .await
        .expect("unbound messenger should be present");

    // Create receptor to act as policy endpoint.
    let mut policy_receptor = messenger_factory
        .create(MessengerType::Addressable(policy_handler_address))
        .await
        .expect("addressable messenger should be present")
        .1;

    let request_payload = internal::policy::Payload::Request(Request::Audio(AudioRequest::Get));

    // Send request.
    let mut reply_receptor = messenger
        .message(request_payload.clone(), Audience::Address(policy_handler_address))
        .send();

    // Wait and verify request received.
    let (payload, client) = policy_receptor.next_payload().await.expect("should receive message");
    assert_eq!(payload, request_payload);
    assert_eq!(client.get_author(), receptor.get_signature());

    let state = StateBuilder::new()
        .add_property(AudioStreamType::Background, TransformFlags::TRANSFORM_MAX)
        .build();

    // Send response.
    let reply_payload =
        internal::policy::Payload::Response(Ok(Payload::Audio(AudioResponse::State(state))));
    client.reply(reply_payload.clone()).send().ack();

    // Verify response received.
    let (result_payload, _) = reply_receptor.next_payload().await.expect("should receive result");
    assert_eq!(result_payload, reply_payload);
}
