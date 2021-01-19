// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::base::SettingInfo;
use crate::handler::base::{Request, SettingHandlerResult};
use crate::handler::device_storage::DeviceStorageCompatible;
use crate::handler::setting_handler::persist::{
    controller as data_controller, write, ClientProxy, WriteResult,
};
use crate::handler::setting_handler::{controller, ControllerError};
use crate::setup::types::{ConfigurationInterfaceFlags, SetupInfo};
use async_trait::async_trait;

impl DeviceStorageCompatible for SetupInfo {
    const KEY: &'static str = "setup_info";

    fn default_value() -> Self {
        SetupInfo { configuration_interfaces: ConfigurationInterfaceFlags::DEFAULT }
    }
}

impl Into<SettingInfo> for SetupInfo {
    fn into(self) -> SettingInfo {
        SettingInfo::Setup(self)
    }
}

pub struct SetupController {
    client: ClientProxy<SetupInfo>,
}

#[async_trait]
impl data_controller::Create<SetupInfo> for SetupController {
    /// Creates the controller
    async fn create(client: ClientProxy<SetupInfo>) -> Result<Self, ControllerError> {
        Ok(Self { client: client })
    }
}

#[async_trait]
impl controller::Handle for SetupController {
    async fn handle(&self, request: Request) -> Option<SettingHandlerResult> {
        match request {
            Request::SetConfigurationInterfaces(interfaces) => {
                let mut info = self.client.read().await;
                info.configuration_interfaces = interfaces;

                return Some(write(&self.client, info, true).await.into_handler_result());
            }
            Request::Get => {
                return Some(Ok(Some(SettingInfo::Setup(self.client.read().await))));
            }
            _ => None,
        }
    }
}
