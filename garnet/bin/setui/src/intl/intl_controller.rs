// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use std::collections::HashSet;
use std::sync::Arc;

use fuchsia_async as fasync;
use fuchsia_syslog::fx_log_err;
use futures::lock::Mutex;
use futures::StreamExt;
use parking_lot::RwLock;

use rust_icu_uenum as uenum;

use crate::registry::base::{Command, Context, Notifier, SettingHandler, State};
use crate::registry::device_storage::DeviceStorageFactory;
use crate::registry::device_storage::{DeviceStorage, DeviceStorageCompatible};
use crate::service_context::ServiceContextHandle;
use crate::switchboard::base::{
    Merge, SettingRequest, SettingRequestResponder, SettingResponse, SettingType, SwitchboardError,
};
use crate::switchboard::intl_types::{HourCycle, IntlInfo, LocaleId, TemperatureUnit};

type IntlStorage = Arc<Mutex<DeviceStorage<IntlInfo>>>;

impl DeviceStorageCompatible for IntlInfo {
    const KEY: &'static str = "intl_info";

    fn default_value() -> Self {
        IntlInfo {
            locales: Some(vec![LocaleId { id: "en-US".to_string() }]),
            temperature_unit: Some(TemperatureUnit::Celsius),
            time_zone_id: Some("UTC".to_string()),
            hour_cycle: Some(HourCycle::H12),
        }
    }
}

pub struct IntlController {
    service_context_handle: ServiceContextHandle,
    stored_value: IntlInfo,
    listen_notifier: Arc<RwLock<Option<Notifier>>>,
    storage: IntlStorage,
    time_zone_ids: std::collections::HashSet<String>,
}

/// Controller for processing switchboard messages surrounding the Intl
/// protocol, backed by a number of services, including TimeZone.
impl IntlController {
    pub fn spawn<T: DeviceStorageFactory + Send + Sync + 'static>(
        context: &Context<T>,
    ) -> SettingHandler {
        let service_context_handle = context.service_context_handle.clone();
        let storage_handle = context.storage_factory_handle.clone();

        let (ctrl_tx, mut ctrl_rx) = futures::channel::mpsc::unbounded::<Command>();

        fasync::spawn(async move {
            let storage = storage_handle.lock().await.get_store::<IntlInfo>();
            // Local copy of persisted i18n values.
            let stored_value: IntlInfo;
            {
                let mut storage_lock = storage.lock().await;
                stored_value = storage_lock.get().await;
            }

            let time_zone_ids = IntlController::load_time_zones();

            let handle = Arc::new(RwLock::new(Self {
                service_context_handle,
                stored_value,
                listen_notifier: Arc::new(RwLock::new(None)),
                storage,
                time_zone_ids,
            }));

            while let Some(command) = ctrl_rx.next().await {
                handle.write().process_command(command);
            }
        });

        return ctrl_tx;
    }

    /// Loads the set of valid time zones from resources.
    fn load_time_zones() -> std::collections::HashSet<String> {
        let _icu_data_loader = icu_data::Loader::new().expect("icu data loaded");
        let mut time_zone_set = HashSet::new();

        let time_zone_list = match uenum::open_time_zones() {
            Ok(time_zones) => time_zones,
            Err(err) => {
                fx_log_err!("Unable to load time zones: {:?}", err);
                return time_zone_set;
            }
        };

        for time_zone_result in time_zone_list {
            if let Ok(time_zone_id) = time_zone_result {
                time_zone_set.insert(time_zone_id);
            }
        }

        time_zone_set
    }

    fn process_command(&mut self, command: Command) {
        match command {
            Command::ChangeState(state) => match state {
                State::Listen(notifier) => {
                    *self.listen_notifier.write() = Some(notifier);
                }
                State::EndListen => {
                    *self.listen_notifier.write() = None;
                }
            },
            Command::HandleRequest(request, responder) => match request {
                SettingRequest::SetIntlInfo(info) => {
                    self.set(info, responder);
                }
                SettingRequest::Get => {
                    self.get(responder);
                }
                _ => {
                    responder
                        .send(Err(SwitchboardError::UnimplementedRequest {
                            setting_type: SettingType::Intl,
                            request: request,
                        }))
                        .ok();
                }
            },
        }
    }

    fn get(&self, responder: SettingRequestResponder) {
        let _ = responder.send(Ok(Some(SettingResponse::Intl(self.stored_value.clone()))));
    }

    fn set(&mut self, info: IntlInfo, responder: SettingRequestResponder) {
        if let Err(err) = self.validate_intl_info(info.clone()) {
            fx_log_err!("Invalid IntlInfo provided: {:?}", err);
            let _ = responder.send(Err(err));
            return;
        }

        self.write_intl_info_to_service(info.clone());
        self.write_intl_info_to_local_storage(info.clone(), responder);
    }

    /// Checks if the given IntlInfo is valid.
    fn validate_intl_info(&self, info: IntlInfo) -> Result<(), SwitchboardError> {
        if let Some(time_zone_id) = info.time_zone_id {
            // Make sure the given time zone ID is valid.
            if !self.time_zone_ids.contains(time_zone_id.as_str()) {
                return Err(SwitchboardError::InvalidArgument {
                    setting_type: SettingType::Intl,
                    argument: "timezone id".to_string(),
                    value: time_zone_id.as_str().to_string(),
                });
            }
        }

        Ok(())
    }

    /// Writes the time zone setting to the timezone service.
    ///
    /// Errors are only logged as this is an intermediate step in a migration.
    /// TODO(fxb/41639): remove this
    fn write_intl_info_to_service(&self, info: IntlInfo) {
        let service_context = self.service_context_handle.clone();
        fasync::spawn(async move {
            let service_result = service_context
                .lock()
                .await
                .connect::<fidl_fuchsia_deprecatedtimezone::TimezoneMarker>();

            if service_result.is_err() {
                fx_log_err!("Failed to connect to fuchsia.timezone");
                return;
            }

            let time_zone_id = match info.time_zone_id {
                Some(id) => id,
                None => return,
            };

            let proxy = service_result.unwrap();

            if let Err(e) = proxy.set_timezone(time_zone_id.as_str()).await {
                fx_log_err!("Failed to write timezone to fuchsia.timezone: {:?}", e);
            }
        });
    }

    /// Writes the intl info to persistent storage and updates our local copy.
    ///
    /// TODO(fxb/41639): inline this method into set_time_zone
    fn write_intl_info_to_local_storage(
        &mut self,
        info: IntlInfo,
        responder: SettingRequestResponder,
    ) {
        let old_value = self.stored_value.clone();

        self.stored_value = info.merge(self.stored_value.clone());

        if old_value == self.stored_value {
            // Value unchanged, no need to persist or notify listeners.
            responder.send(Ok(None)).ok();
            return;
        }

        // Attempt to persist the value.
        self.persist_intl_info(self.stored_value.clone(), responder);

        // Notify listeners of value change.
        if let Some(notifier) = (*self.listen_notifier.read()).clone() {
            let _ = notifier.unbounded_send(SettingType::Intl);
        }
    }

    /// Writes the intl info to persistent storage.
    ///
    /// TODO(fxb/41639): inline this method into set_time_zone
    fn persist_intl_info(&self, info: IntlInfo, responder: SettingRequestResponder) {
        let storage_clone = self.storage.clone();
        // Spin off a separate thread to persist the value.
        fasync::spawn(async move {
            let mut storage_lock = storage_clone.lock().await;
            let write_request = storage_lock.write(&info, false).await;
            let _ = match write_request {
                Ok(_) => responder.send(Ok(None)),
                Err(_) => responder.send(Err(SwitchboardError::StorageFailure {
                    setting_type: SettingType::Intl,
                })),
            };
        });
    }
}
