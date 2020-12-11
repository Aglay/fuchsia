// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::convert::TryFrom;
use std::sync::atomic::{AtomicU32, Ordering};

use fidl_fuchsia_media::AudioRenderUsage;
use fidl_fuchsia_settings_policy::{Disable, Mute, PolicyParameters, Volume};

use bitflags::bitflags;

use crate::handler::device_storage::DeviceStorageCompatible;
use crate::switchboard::base::AudioStreamType;

pub mod audio_policy_handler;
pub mod volume_policy_fidl_handler;

pub type PropertyTarget = AudioStreamType;

/// Unique identifier for a policy.
#[derive(Debug, Copy, Clone, Hash, Eq, PartialEq, Serialize, Deserialize)]
pub struct PolicyId(u32);

impl PolicyId {
    pub fn create(policy_id: u32) -> Self {
        Self(policy_id)
    }
}

/// `StateBuilder` is used to construct a new [`State`] as the internal
/// modification of properties should not be available post construction.
///
/// [`State`]: struct.State.html
pub struct StateBuilder {
    properties: HashMap<PropertyTarget, Property>,
}

// TODO(fxbug.dev/60963): remove once used
#[allow(dead_code)]
impl StateBuilder {
    pub fn new() -> Self {
        Self { properties: HashMap::new() }
    }

    pub fn add_property(
        mut self,
        stream_type: PropertyTarget,
        available_transforms: TransformFlags,
    ) -> Self {
        let property = Property::new(stream_type, available_transforms);
        self.properties.insert(stream_type, property);

        self
    }

    pub fn build(self) -> State {
        State { properties: self.properties }
    }
}

/// `State` defines the current configuration of the audio policy. This
/// includes the available properties, which encompass the active transform
/// policies and transforms available to be set.
#[derive(PartialEq, Debug, Clone, Serialize, Deserialize)]
pub struct State {
    properties: HashMap<PropertyTarget, Property>,
}

impl State {
    pub fn get_properties(&self) -> Vec<Property> {
        self.properties.values().cloned().collect::<Vec<Property>>()
    }

    #[cfg(test)]
    pub fn properties(&mut self) -> &mut HashMap<PropertyTarget, Property> {
        &mut self.properties
    }

    /// Attempts to remove the policy with the given ID from the state. Returns the policy if it
    /// was found and removed, else returns None.
    pub fn remove_policy(&mut self, policy_id: PolicyId) -> Option<Policy> {
        for property in self.properties.values_mut() {
            match property.remove_policy(policy_id) {
                Some(policy) => {
                    return Some(policy);
                }
                None => {}
            }
        }
        None
    }
}

impl DeviceStorageCompatible for State {
    fn default_value() -> Self {
        State { properties: Default::default() }
    }

    const KEY: &'static str = "audio_policy_state";
}

/// `Property` defines the current policy configuration over a given audio
/// stream type.
#[derive(PartialEq, Debug, Clone, Serialize, Deserialize)]
pub struct Property {
    /// Identifier used to reference this type over other requests, such as
    /// setting a policy.
    pub target: PropertyTarget,
    /// The stream type uniquely identifies the type of stream.
    pub stream_type: AudioStreamType,
    /// The available transforms provided as a bitmask.
    pub available_transforms: TransformFlags,
    /// The active transform definitions on this stream type.
    pub active_policies: Vec<Policy>,
}

impl Property {
    pub fn new(stream_type: AudioStreamType, available_transforms: TransformFlags) -> Self {
        Self { target: stream_type, stream_type, available_transforms, active_policies: vec![] }
    }

    /// Adds the given transform to this property and returns its PolicyId.
    pub fn add_transform(&mut self, transform: Transform) -> PolicyId {
        // Static atomic counter ensures each policy has a unique ID. Declared locally so nothing
        // else has access.
        static POLICY_ID_COUNTER: AtomicU32 = AtomicU32::new(1);
        let policy_id = PolicyId(POLICY_ID_COUNTER.fetch_add(1, Ordering::Relaxed));
        let policy = Policy { id: policy_id, transform };

        self.active_policies.push(policy);

        policy_id
    }

    /// Attempts to remove the policy with the given ID from this property. Returns the policy if it
    /// was found and removed, else returns None.
    pub fn remove_policy(&mut self, policy_id: PolicyId) -> Option<Policy> {
        match self.active_policies.iter().position(|policy| policy.id == policy_id) {
            Some(index) => Some(self.active_policies.remove(index)),
            None => None,
        }
    }
}

impl From<Property> for fidl_fuchsia_settings_policy::Property {
    fn from(src: Property) -> Self {
        fidl_fuchsia_settings_policy::Property {
            target: Some(src.stream_type.into()),
            available_transforms: Some(src.available_transforms.into()),
            active_policies: Some(
                src.active_policies.into_iter().map(Policy::into).collect::<Vec<_>>(),
            ),
            ..fidl_fuchsia_settings_policy::Property::EMPTY
        }
    }
}

impl From<fidl_fuchsia_settings_policy::Target> for AudioStreamType {
    fn from(src: fidl_fuchsia_settings_policy::Target) -> Self {
        match src {
            fidl_fuchsia_settings_policy::Target::Stream(AudioRenderUsage::Background) => {
                AudioStreamType::Background
            }
            fidl_fuchsia_settings_policy::Target::Stream(AudioRenderUsage::Media) => {
                AudioStreamType::Media
            }
            fidl_fuchsia_settings_policy::Target::Stream(AudioRenderUsage::Interruption) => {
                AudioStreamType::Interruption
            }
            fidl_fuchsia_settings_policy::Target::Stream(AudioRenderUsage::SystemAgent) => {
                AudioStreamType::SystemAgent
            }
            fidl_fuchsia_settings_policy::Target::Stream(AudioRenderUsage::Communication) => {
                AudioStreamType::Communication
            }
        }
    }
}

impl From<AudioStreamType> for fidl_fuchsia_settings_policy::Target {
    fn from(src: AudioStreamType) -> Self {
        match src {
            AudioStreamType::Background => {
                fidl_fuchsia_settings_policy::Target::Stream(AudioRenderUsage::Background)
            }
            AudioStreamType::Media => {
                fidl_fuchsia_settings_policy::Target::Stream(AudioRenderUsage::Media)
            }
            AudioStreamType::Interruption => {
                fidl_fuchsia_settings_policy::Target::Stream(AudioRenderUsage::Interruption)
            }
            AudioStreamType::SystemAgent => {
                fidl_fuchsia_settings_policy::Target::Stream(AudioRenderUsage::SystemAgent)
            }
            AudioStreamType::Communication => {
                fidl_fuchsia_settings_policy::Target::Stream(AudioRenderUsage::Communication)
            }
        }
    }
}

bitflags! {
    /// `TransformFlags` defines the available transform space.
    #[derive(Serialize, Deserialize)]
    pub struct TransformFlags: u64 {
        const TRANSFORM_MAX = 1 << 0;
        const TRANSFORM_MIN = 1 << 1;
        const TRANSFORM_MUTE = 1 << 2;
        const TRANSFORM_DISABLE = 1 << 3;
    }
}

impl From<TransformFlags> for Vec<fidl_fuchsia_settings_policy::Transform> {
    fn from(src: TransformFlags) -> Self {
        let mut transforms = Vec::new();
        if src.contains(TransformFlags::TRANSFORM_MAX) {
            transforms.push(fidl_fuchsia_settings_policy::Transform::Max);
        }
        if src.contains(TransformFlags::TRANSFORM_MIN) {
            transforms.push(fidl_fuchsia_settings_policy::Transform::Min);
        }
        if src.contains(TransformFlags::TRANSFORM_MUTE) {
            transforms.push(fidl_fuchsia_settings_policy::Transform::Mute);
        }
        if src.contains(TransformFlags::TRANSFORM_DISABLE) {
            transforms.push(fidl_fuchsia_settings_policy::Transform::Disable);
        }
        return transforms;
    }
}

/// `Policy` captures a fully specified transform.
#[derive(PartialEq, Debug, Clone, Serialize, Deserialize)]
pub struct Policy {
    pub id: PolicyId,
    pub transform: Transform,
}

impl From<Policy> for fidl_fuchsia_settings_policy::Policy {
    fn from(src: Policy) -> Self {
        fidl_fuchsia_settings_policy::Policy {
            policy_id: Some(src.id.0),
            parameters: Some(src.transform.into()),
            ..fidl_fuchsia_settings_policy::Policy::EMPTY
        }
    }
}

/// `Transform` provides the parameters for specifying a transform.
#[derive(PartialEq, Debug, Clone, Copy, Serialize, Deserialize)]
pub enum Transform {
    // Limits the maximum volume an audio stream can be set to.
    Max(f32),
    // Sets a floor for the minimum volume an audio stream can be set to.
    Min(f32),
    // Locks the audio stream mute status to the given value.
    Mute(bool),
    // Rejects all sets to the given audio stream.
    Disable,
}

impl TryFrom<PolicyParameters> for Transform {
    type Error = &'static str;

    fn try_from(src: PolicyParameters) -> Result<Self, Self::Error> {
        Ok(match src {
            PolicyParameters::Max(Volume { volume, .. }) => {
                Transform::Max(volume.ok_or_else(|| "missing max volume")?)
            }
            PolicyParameters::Min(Volume { volume, .. }) => {
                Transform::Min(volume.ok_or_else(|| "missing min volume")?)
            }
            PolicyParameters::Mute(Mute { mute, .. }) => {
                Transform::Mute(mute.ok_or_else(|| "missing mute state")?)
            }
            PolicyParameters::Disable(_) => Transform::Disable,
        })
    }
}

impl From<Transform> for PolicyParameters {
    fn from(src: Transform) -> Self {
        match src {
            Transform::Max(vol) => {
                PolicyParameters::Max(Volume { volume: Some(vol), ..Volume::EMPTY })
            }
            Transform::Min(vol) => {
                PolicyParameters::Min(Volume { volume: Some(vol), ..Volume::EMPTY })
            }
            Transform::Mute(is_muted) => {
                PolicyParameters::Mute(Mute { mute: Some(is_muted), ..Mute::EMPTY })
            }
            Transform::Disable => PolicyParameters::Disable(Disable::EMPTY),
        }
    }
}

/// Available requests to interact with the volume policy.
#[derive(PartialEq, Clone, Debug)]
pub enum Request {
    /// Fetches the current policy state.
    Get,
    /// Adds a policy transform to the specified property. If successful, this transform will become
    /// a policy on the property.
    AddPolicy(PropertyTarget, Transform),
    /// Removes an existing policy on the property.
    RemovePolicy(PolicyId),
}

/// Successful responses for [`Request`]
///
/// [`Request`]: enum.Request.html
#[derive(PartialEq, Clone, Debug)]
pub enum Response {
    /// Response to any transform addition or policy removal. The returned id
    /// represents the modified policy.
    Policy(PolicyId),
    /// Response to request for state.
    State(State),
}

#[cfg(test)]
mod tests {
    use crate::audio::policy::{
        PolicyId, Property, PropertyTarget, StateBuilder, Transform, TransformFlags,
    };
    use crate::switchboard::base::AudioStreamType;
    use fidl_fuchsia_settings_policy::{Disable, Mute, PolicyParameters, Volume};
    use matches::assert_matches;
    use std::collections::{HashMap, HashSet};
    use std::convert::TryFrom;
    use std::iter::FromIterator;

    /// Verifies that using `TryFrom` to convert a `PolicyParameters` into a `Transform` will fail
    /// if the source did not have required parameters specified.
    #[test]
    fn parameter_to_transform_missing_arguments() {
        let max_params = PolicyParameters::Max(Volume { volume: None, ..Volume::EMPTY });
        let min_params = PolicyParameters::Min(Volume { volume: None, ..Volume::EMPTY });
        let mute_params = PolicyParameters::Mute(Mute { mute: None, ..Mute::EMPTY });

        assert_matches!(Transform::try_from(max_params), Err(_));
        assert_matches!(Transform::try_from(min_params), Err(_));
        assert_matches!(Transform::try_from(mute_params), Err(_));
    }

    /// Verifies that using `TryFrom` to convert a `PolicyParameters` into a `Transform` succeeds
    /// and that the result contains the same parameters as the source.
    #[test]
    fn parameter_to_transform() {
        let max_volume = 0.5;
        let min_volume = 0.5;
        let mute = true;
        let max_params =
            PolicyParameters::Max(Volume { volume: Some(max_volume), ..Volume::EMPTY });
        let min_params =
            PolicyParameters::Min(Volume { volume: Some(min_volume), ..Volume::EMPTY });
        let mute_params = PolicyParameters::Mute(Mute { mute: Some(mute), ..Mute::EMPTY });
        let disable_params = PolicyParameters::Disable(Disable::EMPTY);

        assert_eq!(Transform::try_from(max_params), Ok(Transform::Max(max_volume)));
        assert_eq!(Transform::try_from(min_params), Ok(Transform::Min(min_volume)));
        assert_eq!(Transform::try_from(mute_params), Ok(Transform::Mute(mute)));
        assert_eq!(Transform::try_from(disable_params), Ok(Transform::Disable));
    }

    // Verifies that the audio policy state builder functions correctly for adding targets and
    // transforms.
    #[test]
    fn test_state_builder() {
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
                *properties.get(&property.stream_type).expect("unexpected property")
            );
        }
    }

    // Verifies that adding transforms to policy properties works.
    #[test]
    fn test_property_transforms() {
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
}
