// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use std::collections::HashSet;

use fuchsia_syslog::fx_log_warn;
use thiserror::Error;

use crate::base::{SettingInfo, SettingType};
use crate::handler::base::{Request, SettingHandlerResult};
use crate::handler::setting_handler::ControllerError;
use std::borrow::Cow;

/// Return type from a controller after handling a state change.
pub type ControllerStateResult = Result<(), ControllerError>;
pub type SettingResponseResult = Result<Option<SettingInfo>, SwitchboardError>;

/// A trait for structs where all fields are options. Recursively performs
/// [Option::or](std::option::Option::or) on each field in the struct and substructs.
pub trait Merge {
    fn merge(&self, other: Self) -> Self;
}

#[derive(Error, Debug, Clone, PartialEq)]
pub enum SwitchboardError {
    #[error("Unimplemented Request:{0:?} for setting type: {1:?}")]
    UnimplementedRequest(SettingType, Request),

    #[error("Storage failure for setting type: {0:?}")]
    StorageFailure(SettingType),

    #[error("Initialization failure: cause {0:?}")]
    InitFailure(Cow<'static, str>),

    #[error("Restoration of setting on controller startup failed: cause {0:?}")]
    RestoreFailure(Cow<'static, str>),

    #[error("Invalid argument for setting type: {0:?} argument:{1:?} value:{2:?}")]
    InvalidArgument(SettingType, Cow<'static, str>, Cow<'static, str>),

    #[error("External failure for setting type:{0:?} dependency: {1:?} request:{2:?}")]
    ExternalFailure(SettingType, Cow<'static, str>, Cow<'static, str>),

    #[error("Unhandled type: {0:?}")]
    UnhandledType(SettingType),

    #[error("Delivery error for type: {0:?} received by: {1:?}")]
    DeliveryError(SettingType, SettingType),

    #[error("Unexpected error: {0}")]
    UnexpectedError(Cow<'static, str>),

    #[error("Undeliverable Request:{1:?} for setting type: {0:?}")]
    UndeliverableError(SettingType, Request),

    #[error("Unsupported request for setting type: {0:?}")]
    UnsupportedError(SettingType),

    #[error("Communication error")]
    CommunicationError,

    #[error("Irrecoverable error")]
    IrrecoverableError,

    #[error("Timeout error")]
    TimeoutError,
}

impl From<ControllerError> for SwitchboardError {
    fn from(error: ControllerError) -> Self {
        match error {
            ControllerError::UnimplementedRequest(setting_type, request) => {
                SwitchboardError::UnimplementedRequest(setting_type, request)
            }
            ControllerError::WriteFailure(setting_type) => {
                SwitchboardError::StorageFailure(setting_type)
            }
            ControllerError::InitFailure(description) => SwitchboardError::InitFailure(description),
            ControllerError::RestoreFailure(description) => {
                SwitchboardError::RestoreFailure(description)
            }
            ControllerError::ExternalFailure(setting_type, dependency, request) => {
                SwitchboardError::ExternalFailure(setting_type, dependency, request)
            }
            ControllerError::InvalidArgument(setting_type, argument, value) => {
                SwitchboardError::InvalidArgument(setting_type, argument, value)
            }
            ControllerError::UnhandledType(setting_type) => {
                SwitchboardError::UnhandledType(setting_type)
            }
            ControllerError::UnexpectedError(error) => SwitchboardError::UnexpectedError(error),
            ControllerError::UndeliverableError(setting_type, request) => {
                SwitchboardError::UndeliverableError(setting_type, request)
            }
            ControllerError::UnsupportedError(setting_type) => {
                SwitchboardError::UnsupportedError(setting_type)
            }
            ControllerError::DeliveryError(setting_type, setting_type_2) => {
                SwitchboardError::DeliveryError(setting_type, setting_type_2)
            }
            ControllerError::IrrecoverableError => SwitchboardError::IrrecoverableError,
            ControllerError::TimeoutError => SwitchboardError::TimeoutError,
            ControllerError::ExitError => SwitchboardError::IrrecoverableError,
        }
    }
}

/// Returns all known setting types. New additions to SettingType should also
/// be inserted here.
pub fn get_all_setting_types() -> HashSet<SettingType> {
    return vec![
        SettingType::Accessibility,
        SettingType::Audio,
        SettingType::Device,
        SettingType::Display,
        SettingType::DoNotDisturb,
        SettingType::FactoryReset,
        SettingType::Input,
        SettingType::Intl,
        SettingType::Light,
        SettingType::LightSensor,
        SettingType::NightMode,
        SettingType::Power,
        SettingType::Privacy,
        SettingType::Setup,
    ]
    .into_iter()
    .collect();
}

/// Returns default setting types. These types should be product-agnostic,
/// capable of operating with platform level support.
pub fn get_default_setting_types() -> HashSet<SettingType> {
    return vec![
        SettingType::Accessibility,
        SettingType::Device,
        SettingType::Intl,
        SettingType::Power,
        SettingType::Privacy,
        SettingType::Setup,
    ]
    .into_iter()
    .collect();
}

/// Description of an action request on a setting. This wraps a
/// SettingActionData, providing destination details (setting type) along with
/// callback information (action id).
#[derive(PartialEq, Debug, Clone)]
pub struct SettingAction {
    pub id: u64,
    pub setting_type: SettingType,
    pub data: SettingActionData,
}

/// The types of actions. Note that specific request types should be enumerated
/// in the Request enum.
#[derive(PartialEq, Debug, Clone)]
pub enum SettingActionData {
    /// The listening state has changed for the particular setting. The provided
    /// value indicates the number of active listeners. 0 indicates there are
    /// no more listeners.
    Listen(u64),
    /// A request has been made on a particular setting. The specific setting
    /// and request data are encoded in Request.
    Request(Request),
}

/// The events generated in response to SettingAction.
#[derive(PartialEq, Clone, Debug)]
pub enum SettingEvent {
    /// The setting's data has changed. The setting type associated with this
    /// event is implied by the association of the signature of the sending
    /// proxy to the setting type. This mapping is maintained by the
    /// Switchboard. The [`SettingInfo`] provided represents the most up-to-date
    /// data at the time of this event.
    Changed(SettingInfo),
    /// A response to a previous SettingActionData::Request is ready. The source
    /// SettingAction's id is provided alongside the result.
    Response(u64, SettingHandlerResult),
}

/// Custom trait used to handle results from responding to FIDL calls.
pub trait FidlResponseErrorLogger {
    fn log_fidl_response_error(&self, client_name: &str);
}

/// In order to not crash when a client dies, logs but doesn't crash for the specific case of
/// being unable to write to the client. Crashes if other types of errors occur.
impl FidlResponseErrorLogger for Result<(), fidl::Error> {
    fn log_fidl_response_error(&self, client_name: &str) {
        if let Some(error) = self.as_ref().err() {
            match error {
                fidl::Error::ServerResponseWrite(_) => {
                    fx_log_warn!("Failed to respond to client {:?} : {:?}", client_name, error);
                }
                _ => {
                    panic!(
                        "Unexpected client response error from client {:?} : {:?}",
                        client_name, error
                    );
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use fuchsia_zircon as zx;

    use super::*;

    /// Should succeed either when responding was successful or there was an error on the client side.
    #[test]
    fn test_error_logger_succeeds() {
        let result = Err(fidl::Error::ServerResponseWrite(zx::Status::PEER_CLOSED));
        result.log_fidl_response_error("");

        let result = Ok(());
        result.log_fidl_response_error("");
    }

    /// Should fail at all other times.
    #[should_panic]
    #[test]
    fn test_error_logger_fails() {
        let result = Err(fidl::Error::ServerRequestRead(zx::Status::PEER_CLOSED));
        result.log_fidl_response_error("");
    }
}
