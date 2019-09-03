// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use bitflags::bitflags;
use failure::Error;
use futures::channel::mpsc::UnboundedSender;
use futures::channel::oneshot::Sender;
use serde_derive::{Deserialize, Serialize};
use std::collections::HashSet;

pub type SettingResponseResult = Result<Option<SettingResponse>, Error>;
pub type SettingRequestResponder = Sender<SettingResponseResult>;

/// The setting types supported by the messaging system. This is used as a key
/// for listening to change notifications and sending requests.
/// The types are arranged alphabetically.
#[derive(PartialEq, Debug, Eq, Hash, Clone, Copy)]
pub enum SettingType {
    Unknown,
    Accessibility,
    Audio,
    Display,
    DoNotDisturb,
    Intl,
    Setup,
    System,
}

/// Returns all known setting types. New additions to SettingType should also
/// be inserted here.
pub fn get_all_setting_types() -> HashSet<SettingType> {
    let mut set = HashSet::new();
    set.insert(SettingType::Accessibility);
    set.insert(SettingType::Audio);
    set.insert(SettingType::Display);
    set.insert(SettingType::DoNotDisturb);
    set.insert(SettingType::Intl);
    set.insert(SettingType::Setup);
    set.insert(SettingType::System);

    set
}

/// The possible requests that can be made on a setting. The sink will expect a
/// subset of the values defined below based on the associated type.
/// The types are arranged alphabetically.
#[derive(PartialEq, Debug, Clone)]
pub enum SettingRequest {
    Get,

    // Accessibility requests.
    SetAudioDescription(bool),
    SetColorCorrection(ColorBlindnessType),

    // Audio requests.
    SetVolume(Vec<AudioStream>),

    // Display requests.
    SetBrightness(f32),
    SetAutoBrightness(bool),

    // System login requests.
    SetLoginOverrideMode(SystemLoginOverrideMode),

    // Intl requests.
    SetTimeZone(String),

    // Setup info requests.
    SetConfigurationInterfaces(ConfigurationInterfaceFlags),

    // Do not disturb requests.
    SetUserInitiatedDoNotDisturb(bool),
    SetNightModeInitiatedDoNotDisturb(bool),
}

#[derive(PartialEq, Debug, Clone, Copy, Serialize, Deserialize)]
pub struct AccessibilityInfo {
    pub audio_description: bool,
    pub color_correction: ColorBlindnessType,
}

#[derive(PartialEq, Debug, Clone)]
pub enum AudioSettingSource {
    Default,
    User,
    System,
}

#[derive(PartialEq, Debug, Clone, Copy, Hash, Eq)]
pub enum AudioStreamType {
    Background,
    Media,
    Interruption,
    SystemAgent,
    Communication,
}

#[derive(PartialEq, Debug, Clone)]
pub struct AudioStream {
    pub stream_type: AudioStreamType,
    pub source: AudioSettingSource,
    pub user_volume_level: f32,
    pub user_volume_muted: bool,
}

// TODO(go/fxb/35873): Add AudioInput support.
#[derive(PartialEq, Debug, Clone)]
pub struct AudioInfo {
    pub streams: Vec<AudioStream>,
}

#[derive(PartialEq, Debug, Clone, Copy, Serialize, Deserialize)]
pub enum ColorBlindnessType {
    /// No color blindness.
    None,

    /// Red-green color blindness due to reduced sensitivity to red light.
    Protanomaly,

    /// Red-green color blindness due to reduced sensitivity to green light.
    Deuteranomaly,

    /// Blue-yellow color blindness. It is due to reduced sensitivity to blue
    /// light.
    Tritanomaly,
}

impl From<fidl_fuchsia_settings::ColorBlindnessType> for ColorBlindnessType {
    fn from(color_blindness_type: fidl_fuchsia_settings::ColorBlindnessType) -> Self {
        match color_blindness_type {
            fidl_fuchsia_settings::ColorBlindnessType::None => ColorBlindnessType::None,
            fidl_fuchsia_settings::ColorBlindnessType::Protanomaly => {
                ColorBlindnessType::Protanomaly
            }
            fidl_fuchsia_settings::ColorBlindnessType::Deuteranomaly => {
                ColorBlindnessType::Deuteranomaly
            }
            fidl_fuchsia_settings::ColorBlindnessType::Tritanomaly => {
                ColorBlindnessType::Tritanomaly
            }
        }
    }
}

impl From<ColorBlindnessType> for fidl_fuchsia_settings::ColorBlindnessType {
    fn from(color_blindness_type: ColorBlindnessType) -> Self {
        match color_blindness_type {
            ColorBlindnessType::None => fidl_fuchsia_settings::ColorBlindnessType::None,
            ColorBlindnessType::Protanomaly => {
                fidl_fuchsia_settings::ColorBlindnessType::Protanomaly
            }
            ColorBlindnessType::Deuteranomaly => {
                fidl_fuchsia_settings::ColorBlindnessType::Deuteranomaly
            }
            ColorBlindnessType::Tritanomaly => {
                fidl_fuchsia_settings::ColorBlindnessType::Tritanomaly
            }
        }
    }
}

#[derive(PartialEq, Debug, Clone, Copy, Serialize, Deserialize)]
pub struct DisplayInfo {
    /// The last brightness value that was manually set
    pub manual_brightness_value: f32,
    pub auto_brightness: bool,
}

impl DisplayInfo {
    pub const fn new(auto_brightness: bool, manual_brightness_value: f32) -> DisplayInfo {
        DisplayInfo {
            manual_brightness_value: manual_brightness_value,
            auto_brightness: auto_brightness,
        }
    }
}

bitflags! {
    #[derive(Serialize, Deserialize)]
    pub struct ConfigurationInterfaceFlags: u32 {
        const ETHERNET = 1 << 0;
        const WIFI = 1 << 1;
        const DEFAULT = Self::ETHERNET.bits | Self::WIFI.bits;
    }
}

#[derive(PartialEq, Debug, Clone, Copy)]
pub struct DoNotDisturbInfo {
    pub user_dnd: bool,
    pub night_mode_dnd: bool,
}

impl DoNotDisturbInfo {
    pub const fn new(user_dnd: bool, night_mode_dnd: bool) -> DoNotDisturbInfo {
        DoNotDisturbInfo { user_dnd: user_dnd, night_mode_dnd: night_mode_dnd }
    }
}

#[derive(PartialEq, Debug, Clone)]
pub struct IntlInfo {
    pub time_zone_id: String,
}

#[derive(PartialEq, Debug, Clone, Copy)]
pub enum SystemLoginOverrideMode {
    None,
    AutologinGuest,
    AuthProvider,
}

#[derive(PartialEq, Debug, Clone)]
pub struct SystemInfo {
    pub login_override_mode: SystemLoginOverrideMode,
}

#[derive(PartialEq, Debug, Clone, Copy, Deserialize, Serialize)]
pub struct SetupInfo {
    pub configuration_interfaces: ConfigurationInterfaceFlags,
}

/// The possible responses to a SettingRequest.
#[derive(PartialEq, Debug, Clone)]
pub enum SettingResponse {
    Unknown,
    Accessibility(AccessibilityInfo),
    Audio(AudioInfo),
    /// Response to a request to get current brightness state.AccessibilityEncoder
    Brightness(DisplayInfo),
    DoNotDisturb(DoNotDisturbInfo),
    Intl(IntlInfo),
    Setup(SetupInfo),
    System(SystemInfo),
}

/// Description of an action request on a setting. This wraps a
/// SettingActionData, providing destination details (setting type) along with
/// callback information (action id).
pub struct SettingAction {
    pub id: u64,
    pub setting_type: SettingType,
    pub data: SettingActionData,
}

/// The types of actions. Note that specific request types should be enumerated
/// in the SettingRequest enum.
#[derive(PartialEq, Debug)]
pub enum SettingActionData {
    /// The listening state has changed for the particular setting. The provided
    /// value indicates the number of active listeners. 0 indicates there are
    /// no more listeners.
    Listen(u64),
    /// A request has been made on a particular setting. The specific setting
    /// and request data are encoded in SettingRequest.
    Request(SettingRequest),
}

/// The events generated in response to SettingAction.
pub enum SettingEvent {
    /// The backing data for the specified setting type has changed. Interested
    /// parties can query through request to get the updated values.
    Changed(SettingType),
    /// A response to a previous SettingActionData::Request is ready. The source
    /// SettingAction's id is provided alongside the result.
    Response(u64, SettingResponseResult),
}

/// A trait handed back from Switchboard's listen interface. Allows client to
/// signal they want to end the session.
pub trait ListenSession: Drop {
    /// Invoked to close the current listening session. No further updates will
    /// be provided to the listener provided at the initial listen call.
    fn close(&mut self);
}

/// A interface for send SettingActions.
pub trait Switchboard {
    /// Transmits a SettingRequest. Results are returned from the passed in
    /// oneshot sender.
    fn request(
        &mut self,
        setting_type: SettingType,
        request: SettingRequest,
        callback: Sender<Result<Option<SettingResponse>, Error>>,
    ) -> Result<(), Error>;

    /// Establishes a continuous callback for change notifications around a
    /// SettingType.
    fn listen(
        &mut self,
        setting_type: SettingType,
        listener: UnboundedSender<SettingType>,
    ) -> Result<Box<dyn ListenSession + Send + Sync>, Error>;
}
