// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
use {
    crate::registry::base::{Command, Context, Notifier, SettingHandler, State},
    crate::registry::device_storage::{DeviceStorageCompatible, DeviceStorageFactory},
    crate::switchboard::base::{
        DisplayInfo, SettingRequest, SettingResponse, SettingType, SwitchboardError,
    },
    anyhow::{format_err, Error},
    fuchsia_async as fasync,
    fuchsia_syslog::fx_log_err,
    futures::lock::Mutex,
    futures::StreamExt,
    parking_lot::RwLock,
    std::sync::Arc,
};

impl DeviceStorageCompatible for DisplayInfo {
    const KEY: &'static str = "display_info";

    fn default_value() -> Self {
        DisplayInfo::new(false /*auto_brightness_enabled*/, 0.5 /*brightness_value*/)
    }
}

pub fn spawn_display_controller<T: DeviceStorageFactory + Send + Sync + 'static>(
    context: &Context<T>,
) -> SettingHandler {
    let storage_handle = context.storage_factory_handle.clone();
    let service_context_handle = context.service_context_handle.clone();
    let (display_handler_tx, mut display_handler_rx) =
        futures::channel::mpsc::unbounded::<Command>();

    let notifier_lock = Arc::<RwLock<Option<Notifier>>>::new(RwLock::new(None));

    fasync::spawn(async move {
        let brightness_service = service_context_handle
            .lock()
            .await
            .connect::<fidl_fuchsia_ui_brightness::ControlMarker>()
            .expect("connected to brightness");

        while let Some(command) = display_handler_rx.next().await {
            match command {
                Command::ChangeState(state) => match state {
                    State::Listen(notifier) => {
                        *notifier_lock.write() = Some(notifier);
                    }
                    State::EndListen => {
                        *notifier_lock.write() = None;
                    }
                },
                Command::HandleRequest(request, responder) => {
                    #[allow(unreachable_patterns)]
                    match request {
                        SettingRequest::Restore => {
                            let storage = storage_handle.lock().await.get_store::<DisplayInfo>();

                            // Load and set value
                            // TODO(fxb/35004): Listen to changes using hanging
                            // get as well
                            let stored_value: DisplayInfo;
                            {
                                let mut storage_lock = storage.lock().await;
                                stored_value = storage_lock.get().await;
                            }
                            match set_brightness(
                                stored_value,
                                &brightness_service,
                                storage_handle.clone(),
                            )
                            .await
                            {
                                Ok(_) => {
                                    responder.send(Ok(None)).unwrap();
                                }
                                Err(e) => {
                                    responder
                                        .send(Err(Error::new(SwitchboardError::ExternalFailure {
                                            setting_type: SettingType::Display,
                                            dependency: "brightness_service".to_string(),
                                            request: "set_brightness".to_string(),
                                            error: format_err!("could not restore values"),
                                        })))
                                        .ok();
                                    fx_log_err!("failed to set brightness: {}", e);
                                }
                            }
                        }
                        SettingRequest::SetBrightness(brightness_value) => {
                            set_brightness(
                                DisplayInfo::new(
                                    false, /*auto_brightness_enabled*/
                                    brightness_value,
                                ),
                                &brightness_service,
                                storage_handle.clone(),
                            )
                            .await
                            .unwrap_or_else(move |e| {
                                fx_log_err!("failed setting brightness_value, {}", e);
                            });

                            responder.send(Ok(None)).unwrap();
                            notify(notifier_lock.clone());
                        }
                        SettingRequest::SetAutoBrightness(auto_brightness_enabled) => {
                            let brightness_value: f32;
                            {
                                let storage =
                                    storage_handle.lock().await.get_store::<DisplayInfo>();
                                let mut storage_lock = storage.lock().await;
                                let stored_value = storage_lock.get().await;
                                brightness_value = stored_value.manual_brightness_value;
                            }

                            set_brightness(
                                DisplayInfo::new(auto_brightness_enabled, brightness_value),
                                &brightness_service,
                                storage_handle.clone(),
                            )
                            .await
                            .unwrap_or_else(move |e| {
                                fx_log_err!("failed setting brightness_value, {}", e);
                            });

                            responder.send(Ok(None)).unwrap();
                            notify(notifier_lock.clone());
                        }
                        SettingRequest::Get => {
                            let storage = storage_handle.lock().await.get_store::<DisplayInfo>();
                            let mut storage_lock = storage.lock().await;
                            responder
                                .send(Ok(Some(SettingResponse::Brightness(
                                    storage_lock.get().await,
                                ))))
                                .unwrap();
                        }
                        _ => {
                            responder
                                .send(Err(Error::new(SwitchboardError::UnimplementedRequest {
                                    setting_type: SettingType::Display,
                                    request: request,
                                })))
                                .ok();
                        }
                    }
                }
            }
        }
    });
    display_handler_tx
}

async fn set_brightness<T: DeviceStorageFactory + Send + Sync + 'static>(
    info: DisplayInfo,
    brightness_service: &fidl_fuchsia_ui_brightness::ControlProxy,
    storage_handle: Arc<Mutex<T>>,
) -> Result<(), fidl::Error> {
    let storage = storage_handle.lock().await.get_store::<DisplayInfo>();
    let mut storage_lock = storage.lock().await;
    storage_lock.write(&info, false).await.unwrap_or_else(move |e| {
        fx_log_err!("failed storing brightness, {}", e);
    });
    if info.auto_brightness {
        brightness_service.set_auto_brightness()
    } else {
        brightness_service.set_manual_brightness(info.manual_brightness_value)
    }
}

// TODO(fxb/35459): watch for changes on current brightness and notify changes
// that way instead.
fn notify(notifier_lock: Arc<RwLock<Option<Notifier>>>) {
    if let Some(notifier) = (*notifier_lock.read()).clone() {
        notifier.unbounded_send(SettingType::Display).unwrap();
    }
}
