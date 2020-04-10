// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
use crate::switchboard::base::SettingResponseResult;

use crate::registry::base::State;
use crate::registry::device_storage::DeviceStorageCompatible;
use crate::registry::setting_handler::persist::{
    controller as data_controller, write, ClientProxy,
};
use crate::registry::setting_handler::{controller, ControllerError};
use crate::switchboard::base::{DoNotDisturbInfo, SettingRequest, SettingResponse};
use async_trait::async_trait;

impl DeviceStorageCompatible for DoNotDisturbInfo {
    const KEY: &'static str = "do_not_disturb_info";

    fn default_value() -> Self {
        DoNotDisturbInfo::new(false, false)
    }
}

pub struct DoNotDisturbController {
    client: ClientProxy<DoNotDisturbInfo>,
}

#[async_trait]
impl data_controller::Create<DoNotDisturbInfo> for DoNotDisturbController {
    /// Creates the controller
    async fn create(client: ClientProxy<DoNotDisturbInfo>) -> Result<Self, ControllerError> {
        Ok(DoNotDisturbController { client: client })
    }
}

#[async_trait]
impl controller::Handle for DoNotDisturbController {
    async fn handle(&self, request: SettingRequest) -> Option<SettingResponseResult> {
        match request {
            SettingRequest::SetDnD(dnd_info) => {
                let mut stored_value = self.client.read().await;
                if dnd_info.user_dnd.is_some() {
                    stored_value.user_dnd = dnd_info.user_dnd;
                }
                if dnd_info.night_mode_dnd.is_some() {
                    stored_value.night_mode_dnd = dnd_info.night_mode_dnd;
                }
                Some(write(&self.client, stored_value, false).await)
            }
            SettingRequest::Get => {
                Some(Ok(Some(SettingResponse::DoNotDisturb(self.client.read().await))))
            }
            _ => None,
        }
    }

    async fn change_state(&mut self, _state: State) {}
}
