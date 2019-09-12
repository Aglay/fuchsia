// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#![feature(async_await)]

use {
    crate::accessibility::spawn_accessibility_controller,
    crate::accessibility::spawn_accessibility_fidl_handler,
    crate::audio::spawn_audio_controller,
    crate::audio::spawn_audio_fidl_handler,
    crate::default_store::DefaultStore,
    crate::device::spawn_device_controller,
    crate::device::spawn_device_fidl_handler,
    crate::display::spawn_display_controller,
    crate::display::spawn_display_fidl_handler,
    crate::do_not_disturb::spawn_do_not_disturb_controller,
    crate::do_not_disturb::spawn_do_not_disturb_fidl_handler,
    crate::intl::intl_controller::IntlController,
    crate::intl::intl_fidl_handler::IntlFidlHandler,
    crate::json_codec::JsonCodec,
    crate::mutation::*,
    crate::privacy::privacy_controller::PrivacyController,
    crate::privacy::privacy_fidl_handler::PrivacyFidlHandler,
    crate::registry::base::Registry,
    crate::registry::device_storage::DeviceStorageFactory,
    crate::registry::registry_impl::RegistryImpl,
    crate::registry::service_context::ServiceContext,
    crate::setting_adapter::{MutationHandler, SettingAdapter},
    crate::setup::setup_controller::SetupController,
    crate::setup::setup_fidl_handler::SetupFidlHandler,
    crate::switchboard::base::{SettingAction, SettingType},
    crate::switchboard::switchboard_impl::SwitchboardImpl,
    crate::system::spawn_system_controller,
    crate::system::spawn_system_fidl_handler,
    fidl_fuchsia_settings::*,
    fuchsia_async as fasync,
    fuchsia_component::server::{ServiceFsDir, ServiceObj},
    fuchsia_syslog::{fx_log_err, fx_log_info},
    log::error,
    setui_handler::SetUIHandler,
    std::collections::HashSet,
    std::sync::{Arc, RwLock},
};

mod accessibility;
mod audio;
mod common;
mod default_store;
mod device;
mod display;
mod do_not_disturb;
mod fidl_clone;
mod intl;
mod json_codec;
mod mutation;
mod privacy;
mod setting_adapter;
mod setui_handler;
mod setup;
mod system;

pub mod registry;
pub mod switchboard;

/// Brings up the settings service fidl environment.
///
/// This method generates the necessary infrastructure to support the settings
/// service (switchboard, registry, etc.) and brings up the components necessary
/// to support the components specified in the components HashSet.
pub fn create_fidl_service<'a, T: DeviceStorageFactory>(
    mut service_dir: ServiceFsDir<ServiceObj<'a, ()>>,
    components: HashSet<switchboard::base::SettingType>,
    service_context_handle: Arc<RwLock<ServiceContext>>,
    storage_factory: Box<T>,
) {
    let (action_tx, action_rx) = futures::channel::mpsc::unbounded::<SettingAction>();
    let unboxed_storage_factory = &storage_factory;

    // Creates switchboard, handed to interface implementations to send messages
    // to handlers.
    let (switchboard_handle, event_tx) = SwitchboardImpl::create(action_tx);

    // Creates registry, used to register handlers for setting types.
    let registry_handle = RegistryImpl::create(event_tx, action_rx);

    let handler = Arc::new(SetUIHandler::new());

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
        Some(fidl_fuchsia_setui::SettingData::Account(fidl_fuchsia_setui::AccountSettings {
            mode: None,
        })),
    )));

    let handler_clone = handler.clone();

    service_dir.add_fidl_service(move |stream: fidl_fuchsia_setui::SetUiServiceRequestStream| {
        let handler_clone = handler_clone.clone();

        fx_log_info!("Connecting to setui_service");
        fasync::spawn(async move {
            handler_clone
                .handle_stream(stream)
                .await
                .unwrap_or_else(|e| error!("Failed to spawn {:?}", e))
        });
    });

    if components.contains(&SettingType::Accessibility) {
        registry_handle
            .write()
            .unwrap()
            .register(
                switchboard::base::SettingType::Accessibility,
                spawn_accessibility_controller(
                    unboxed_storage_factory.get_store::<switchboard::base::AccessibilityInfo>(),
                ),
            )
            .unwrap();

        let switchboard_handle_clone = switchboard_handle.clone();
        service_dir.add_fidl_service(move |stream: AccessibilityRequestStream| {
            spawn_accessibility_fidl_handler(switchboard_handle_clone.clone(), stream);
        });
    }

    if components.contains(&SettingType::Audio) {
        registry_handle
            .write()
            .unwrap()
            .register(
                switchboard::base::SettingType::Audio,
                spawn_audio_controller(service_context_handle.clone()),
            )
            .unwrap();

        let switchboard_handle_clone = switchboard_handle.clone();
        service_dir.add_fidl_service(move |stream: AudioRequestStream| {
            spawn_audio_fidl_handler(switchboard_handle_clone.clone(), stream);
        });
    }

    if components.contains(&SettingType::Device) {
        registry_handle
            .write()
            .unwrap()
            .register(switchboard::base::SettingType::Device, spawn_device_controller())
            .unwrap();

        let switchboard_handle_clone = switchboard_handle.clone();
        service_dir.add_fidl_service(move |stream: DeviceRequestStream| {
            spawn_device_fidl_handler(switchboard_handle_clone.clone(), stream);
        });
    }

    if components.contains(&SettingType::Display) {
        registry_handle
            .write()
            .unwrap()
            .register(
                switchboard::base::SettingType::Display,
                spawn_display_controller(
                    service_context_handle.clone(),
                    unboxed_storage_factory.get_store::<switchboard::base::DisplayInfo>(),
                ),
            )
            .unwrap();

        let switchboard_handle_clone = switchboard_handle.clone();
        service_dir.add_fidl_service(move |stream: DisplayRequestStream| {
            spawn_display_fidl_handler(switchboard_handle_clone.clone(), stream);
        });
    }

    if components.contains(&SettingType::DoNotDisturb) {
        let register_result = registry_handle.write().unwrap().register(
            switchboard::base::SettingType::DoNotDisturb,
            spawn_do_not_disturb_controller(
                unboxed_storage_factory.get_store::<switchboard::base::DoNotDisturbInfo>(),
            ),
        );
        match register_result {
            Ok(_) => {}
            Err(e) => fx_log_err!("failed to register do_not_disturb in registry, {:#?}", e),
        };

        let switchboard_handle_clone = switchboard_handle.clone();
        service_dir.add_fidl_service(move |stream: DoNotDisturbRequestStream| {
            spawn_do_not_disturb_fidl_handler(switchboard_handle_clone.clone(), stream);
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

    if components.contains(&SettingType::Privacy) {
        registry_handle
            .write()
            .unwrap()
            .register(
                switchboard::base::SettingType::Privacy,
                PrivacyController::spawn(
                    unboxed_storage_factory.get_store::<switchboard::base::PrivacyInfo>(),
                )
                .unwrap(),
            )
            .unwrap();

        let switchboard_handle_clone = switchboard_handle.clone();
        service_dir.add_fidl_service(move |stream: PrivacyRequestStream| {
            PrivacyFidlHandler::spawn(switchboard_handle_clone.clone(), stream);
        });
    }

    if components.contains(&SettingType::System) {
        registry_handle
            .write()
            .unwrap()
            .register(switchboard::base::SettingType::System, spawn_system_controller())
            .unwrap();

        let switchboard_handle_clone = switchboard_handle.clone();
        service_dir.add_fidl_service(move |stream: SystemRequestStream| {
            spawn_system_fidl_handler(switchboard_handle_clone.clone(), stream);
        });
    }

    if components.contains(&SettingType::Setup) {
        registry_handle
            .write()
            .unwrap()
            .register(
                switchboard::base::SettingType::Setup,
                SetupController::spawn(
                    unboxed_storage_factory.get_store::<switchboard::base::SetupInfo>(),
                )
                .unwrap(),
            )
            .unwrap();
        let switchboard_handle_clone = switchboard_handle.clone();
        service_dir.add_fidl_service(move |stream: SetupRequestStream| {
            SetupFidlHandler::spawn(switchboard_handle_clone.clone(), stream);
        });
    }
}

#[cfg(test)]
mod tests;
