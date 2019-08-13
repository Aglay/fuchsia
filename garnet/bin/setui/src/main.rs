// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#![feature(async_await)]
#![allow(unreachable_code)]
#![allow(dead_code)]

use {
    crate::default_store::DefaultStore,
    crate::display::spawn_display_controller,
    crate::display::spawn_display_fidl_handler,
    crate::intl::intl_controller::IntlController,
    crate::intl::intl_fidl_handler::IntlFidlHandler,
    crate::json_codec::JsonCodec,
    crate::mutation::*,
    crate::registry::base::Registry,
    crate::registry::registry_impl::RegistryImpl,
    crate::registry::service_context::ServiceContext,
    crate::setting_adapter::{MutationHandler, SettingAdapter},
    crate::switchboard::base::{get_all_setting_types, SettingAction, SettingType},
    crate::switchboard::switchboard_impl::SwitchboardImpl,
    failure::Error,
    fidl_fuchsia_settings::*,
    fidl_fuchsia_setui::*,
    fuchsia_async as fasync,
    fuchsia_component::server::{ServiceFs, ServiceFsDir, ServiceObj},
    fuchsia_syslog::{self as syslog, fx_log_info},
    futures::StreamExt,
    log::error,
    setui_handler::SetUIHandler,
    std::collections::HashSet,
    std::sync::{Arc, RwLock},
    system_handler::SystemStreamHandler,
};

mod common;
mod default_store;
mod display;
mod fidl_clone;
mod intl;
mod json_codec;
mod mutation;
mod registry;
mod setting_adapter;
mod setui_handler;
mod switchboard;
mod system_handler;

fn main() -> Result<(), Error> {
    syslog::init_with_tags(&["setui-service"]).expect("Can't init logger");
    fx_log_info!("Starting setui-service...");

    let mut executor = fasync::Executor::new()?;

    let service_context = Arc::new(RwLock::new(ServiceContext::new(None)));

    let mut fs = ServiceFs::new();

    create_fidl_service(fs.dir("svc"), get_all_setting_types(), service_context);

    fs.take_and_serve_directory_handle()?;
    let () = executor.run_singlethreaded(fs.collect());
    Ok(())
}

/// Brings up the settings service fidl environment.
///
/// This method generates the necessary infrastructure to support the settings
/// service (switchboard, registry, etc.) and brings up the components necessary
/// to support the components specified in the components HashSet.
fn create_fidl_service<'a>(
    mut service_dir: ServiceFsDir<ServiceObj<'a, ()>>,
    components: HashSet<switchboard::base::SettingType>,
    service_context_handle: Arc<RwLock<ServiceContext>>,
) {
    let (action_tx, action_rx) = futures::channel::mpsc::unbounded::<SettingAction>();

    // Creates switchboard, handed to interface implementations to send messages
    // to handlers.
    let (switchboard_handle, event_tx) = SwitchboardImpl::create(action_tx);

    // Creates registry, used to register handlers for setting types.
    let registry_handle = RegistryImpl::create(event_tx, action_rx);

    let handler = Arc::new(SetUIHandler::new());
    let system_handler = Arc::new(SystemStreamHandler::new(handler.clone()));

    // TODO(SU-210): Remove once other adapters are ready.
    handler.register_adapter(Box::new(SettingAdapter::new(
        fidl_fuchsia_setui::SettingType::Unknown,
        Box::new(DefaultStore::new("/data/unknown.dat".to_string(), Box::new(JsonCodec::new()))),
        MutationHandler { process: &process_string_mutation, check_sync: None },
        None,
    )));

    handler.register_adapter(Box::new(SettingAdapter::new(
        fidl_fuchsia_setui::SettingType::Account,
        Box::new(DefaultStore::new("/data/account.dat".to_string(), Box::new(JsonCodec::new()))),
        MutationHandler {
            process: &process_account_mutation,
            check_sync: Some(&should_sync_account_mutation),
        },
        Some(SettingData::Account(AccountSettings { mode: None })),
    )));

    let handler_clone = handler.clone();

    service_dir.add_fidl_service(move |stream: SetUiServiceRequestStream| {
        let handler_clone = handler_clone.clone();

        fx_log_info!("Connecting to setui_service");
        fasync::spawn(async move {
            handler_clone
                .handle_stream(stream)
                .await
                .unwrap_or_else(|e| error!("Failed to spawn {:?}", e))
        });
    });

    // Register for the new settings APIs as well.

    service_dir.add_fidl_service(move |stream: SystemRequestStream| {
        let system_handler_clone = system_handler.clone();
        fx_log_info!("Connecting to System");
        fasync::spawn(async move {
            system_handler_clone
                .handle_system_stream(stream)
                .await
                .unwrap_or_else(|e| error!("Failed to spawn {:?}", e))
        });
    });

    if components.contains(&SettingType::Display) {
        registry_handle
            .write()
            .unwrap()
            .register(
                switchboard::base::SettingType::Display,
                spawn_display_controller(service_context_handle.clone()),
            )
            .unwrap();

        let switchboard_handle_clone = switchboard_handle.clone();
        service_dir.add_fidl_service(move |stream: DisplayRequestStream| {
            spawn_display_fidl_handler(switchboard_handle_clone.clone(), stream);
        });
    }

    if components.contains(&SettingType::Intl) {
        registry_handle
            .write()
            .unwrap()
            .register(
                switchboard::base::SettingType::Intl,
                IntlController::spawn(service_context_handle.clone()).unwrap(),
            )
            .unwrap();

        let switchboard_handle_clone = switchboard_handle.clone();
        service_dir.add_fidl_service(move |stream: IntlRequestStream| {
            IntlFidlHandler::spawn(switchboard_handle_clone.clone(), stream);
        });
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use failure::format_err;
    use fidl::endpoints::{ServerEnd, ServiceMarker};
    use fuchsia_zircon as zx;
    use futures::prelude::*;

    const ENV_NAME: &str = "settings_service_test_environment";

    enum Services {
        Manager(fidl_fuchsia_device_display::ManagerRequestStream),
        Display(DisplayRequestStream),
        Timezone(fidl_fuchsia_timezone::TimezoneRequestStream),
        Intl(IntlRequestStream),
    }

    /// Tests that the FIDL calls result in appropriate commands sent to the switchboard
    /// TODO(ejia): refactor to be more common with main function
    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_display() {
        const STARTING_BRIGHTNESS: f32 = 0.5;
        const CHANGED_BRIGHTNESS: f32 = 0.8;

        let stored_brightness_value: Arc<RwLock<f64>> =
            Arc::new(RwLock::new(STARTING_BRIGHTNESS.into()));

        let stored_brightness_value_clone = stored_brightness_value.clone();

        let service_gen = move |service_name: &str, channel: zx::Channel| {
            if service_name != fidl_fuchsia_device_display::ManagerMarker::NAME {
                return Err(format_err!("unsupported!"));
            }

            let mut manager_stream =
                ServerEnd::<fidl_fuchsia_device_display::ManagerMarker>::new(channel)
                    .into_stream()?;

            let stored_brightness_value_clone = stored_brightness_value_clone.clone();
            fasync::spawn(async move {
                while let Some(req) = manager_stream.try_next().await.unwrap() {
                    #[allow(unreachable_patterns)]
                    match req {
                        fidl_fuchsia_device_display::ManagerRequest::GetBrightness {
                            responder,
                        } => {
                            responder
                                .send(true, (*stored_brightness_value_clone.read().unwrap()).into())
                                .unwrap();
                        }
                        fidl_fuchsia_device_display::ManagerRequest::SetBrightness {
                            brightness,
                            responder,
                        } => {
                            *stored_brightness_value_clone.write().unwrap() = brightness;
                            responder.send(true).unwrap();
                        }
                    }
                }
            });

            Ok(())
        };

        let mut fs = ServiceFs::new();

        create_fidl_service(
            fs.root_dir(),
            [SettingType::Display].iter().cloned().collect(),
            Arc::new(RwLock::new(ServiceContext::new(Some(Box::new(service_gen))))),
        );

        let env = fs.create_salted_nested_environment(ENV_NAME).unwrap();
        fasync::spawn(fs.collect());

        let display_proxy = env.connect_to_service::<DisplayMarker>().unwrap();

        let settings =
            display_proxy.watch().await.expect("watch completed").expect("watch successful");

        assert_eq!(settings.brightness_value, Some(STARTING_BRIGHTNESS));

        let mut display_settings = DisplaySettings::empty();
        display_settings.brightness_value = Some(CHANGED_BRIGHTNESS);
        display_proxy.set(display_settings).await
            .expect("set completed")
            .expect("set successful");

        let settings =
            display_proxy.watch().await.expect("watch completed").expect("watch successful");

        assert_eq!(settings.brightness_value, Some(CHANGED_BRIGHTNESS));
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_intl() {
        const INITIAL_TIME_ZONE: &'static str = "PDT";

        let service_gen = |service_name: &str, channel: zx::Channel| {
            if service_name != fidl_fuchsia_timezone::TimezoneMarker::NAME {
                return Err(format_err!("unsupported!"));
            }

            let mut timezone_stream =
                ServerEnd::<fidl_fuchsia_timezone::TimezoneMarker>::new(channel).into_stream()?;

            fasync::spawn(async move {
                let mut stored_timezone = INITIAL_TIME_ZONE.to_string();
                while let Some(req) = timezone_stream.try_next().await.unwrap() {
                    #[allow(unreachable_patterns)]
                    match req {
                        fidl_fuchsia_timezone::TimezoneRequest::GetTimezoneId { responder } => {
                            responder.send(&stored_timezone).unwrap();
                        }
                        fidl_fuchsia_timezone::TimezoneRequest::SetTimezone {
                            timezone_id,
                            responder,
                        } => {
                            stored_timezone = timezone_id;
                            responder.send(true).unwrap();
                        }
                        _ => {}
                    }
                }
            });
            Ok(())
        };

        let mut fs = ServiceFs::new();

        create_fidl_service(
            fs.root_dir(),
            [SettingType::Intl].iter().cloned().collect(),
            Arc::new(RwLock::new(ServiceContext::new(Some(Box::new(service_gen))))),
        );

        let env = fs.create_salted_nested_environment(ENV_NAME).unwrap();
        fasync::spawn(fs.collect());

        let intl_service = env.connect_to_service::<IntlMarker>().unwrap();
        let settings =
            intl_service.watch().await.expect("watch completed").expect("watch successful");

        if let Some(time_zone_id) = settings.time_zone_id {
            assert_eq!(time_zone_id.id, INITIAL_TIME_ZONE);
        }

        let mut intl_settings = fidl_fuchsia_settings::IntlSettings::empty();
        let updated_timezone = "PDT";

        intl_settings.time_zone_id =
            Some(fidl_fuchsia_intl::TimeZoneId { id: updated_timezone.to_string() });
        intl_service.set(intl_settings).await.expect("set completed").expect("set successful");
        let settings =
            intl_service.watch().await.expect("watch completed").expect("watch successful");
        assert_eq!(
            settings.time_zone_id,
            Some(fidl_fuchsia_intl::TimeZoneId { id: updated_timezone.to_string() })
        );
    }
}
