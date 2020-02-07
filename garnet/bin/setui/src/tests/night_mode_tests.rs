// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#[cfg(test)]
use {
    crate::registry::device_storage::testing::*,
    crate::registry::device_storage::DeviceStorageFactory, crate::switchboard::base::NightModeInfo,
    crate::switchboard::base::SettingType, crate::EnvironmentBuilder, crate::Runtime,
    fidl_fuchsia_settings::*,
};

const ENV_NAME: &str = "settings_service_night_mode_test_environment";

#[fuchsia_async::run_singlethreaded(test)]
async fn test_night_mode() {
    let initial_value = NightModeInfo { night_mode_enabled: None };
    let changed_value = NightModeInfo { night_mode_enabled: Some(true) };

    // Create and fetch a store from device storage so we can read stored value for testing.
    let factory = InMemoryStorageFactory::create_handle();
    let store = factory.lock().await.get_store::<NightModeInfo>();

    let env = EnvironmentBuilder::new(Runtime::Nested(ENV_NAME), factory)
        .settings(&[SettingType::NightMode])
        .spawn_and_get_nested_environment()
        .await
        .unwrap();

    let night_mode_service = env.connect_to_service::<NightModeMarker>().unwrap();
    // Ensure retrieved value matches set value
    let settings = night_mode_service.watch().await.expect("watch completed");
    assert_eq!(settings.night_mode_enabled, initial_value.night_mode_enabled);
    // Ensure setting interface propagates correctly
    let mut night_mode_settings = fidl_fuchsia_settings::NightModeSettings::empty();
    night_mode_settings.night_mode_enabled = Some(true);
    night_mode_service
        .set(night_mode_settings)
        .await
        .expect("set completed")
        .expect("set successful");

    // Verify the value we set is persisted in DeviceStorage.
    let mut store_lock = store.lock().await;
    let retrieved_struct = store_lock.get().await;
    assert_eq!(changed_value, retrieved_struct);

    // Ensure retrieved value matches set value
    let settings = night_mode_service.watch().await.expect("watch completed");
    assert_eq!(settings.night_mode_enabled, changed_value.night_mode_enabled);
}
