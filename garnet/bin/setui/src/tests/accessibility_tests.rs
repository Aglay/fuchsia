// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#[cfg(test)]
use {
    crate::create_fidl_service,
    crate::fidl_clone::FIDLClone,
    crate::registry::device_storage::testing::*,
    crate::registry::device_storage::DeviceStorageFactory,
    crate::registry::service_context::ServiceContext,
    crate::switchboard::base::SettingType,
    crate::switchboard::base::{AccessibilityInfo, ColorBlindnessType},
    fidl_fuchsia_settings::*,
    fidl_fuchsia_ui_types::ColorRgba,
    fuchsia_async as fasync,
    fuchsia_component::server::ServiceFs,
    futures::prelude::*,
    parking_lot::RwLock,
    std::sync::Arc,
};

const ENV_NAME: &str = "settings_service_accessibility_test_environment";

// TODO(fxb/35254): move out of main.rs
async fn create_test_accessibility_env(
    storage_factory: Box<InMemoryStorageFactory>,
) -> AccessibilityProxy {
    let mut fs = ServiceFs::new();

    create_fidl_service(
        fs.root_dir(),
        [SettingType::Accessibility].iter().cloned().collect(),
        Arc::new(RwLock::new(ServiceContext::new(None))),
        storage_factory,
    );

    let env = fs.create_salted_nested_environment(ENV_NAME).unwrap();
    fasync::spawn(fs.collect());

    env.connect_to_service::<AccessibilityMarker>().unwrap()
}

// TODO(fxb/35254): move out of main.rs
#[fuchsia_async::run_singlethreaded(test)]
async fn test_accessibility() {
    const CHANGED_COLOR_BLINDNESS_TYPE: ColorBlindnessType = ColorBlindnessType::Tritanomaly;
    const TEST_COLOR: ColorRgba = ColorRgba { red: 238.0, green: 23.0, blue: 128.0, alpha: 255.0 };
    const CHANGED_FONT_STYLE: CaptionFontStyle = CaptionFontStyle {
        family: Some(CaptionFontFamily::Casual),
        color: Some(TEST_COLOR),
        relative_size: Some(1.0),
        char_edge_style: Some(EdgeStyle::Raised),
    };
    const CHANGED_CAPTION_SETTINGS: CaptionsSettings = CaptionsSettings {
        for_media: Some(true),
        for_tts: Some(true),
        font_style: Some(CHANGED_FONT_STYLE),
        window_color: Some(TEST_COLOR),
        background_color: Some(TEST_COLOR),
    };

    let initial_settings = AccessibilitySettings::empty();

    let mut expected_settings = AccessibilitySettings::empty();
    expected_settings.audio_description = Some(true);
    expected_settings.screen_reader = Some(true);
    expected_settings.color_inversion = Some(true);
    expected_settings.enable_magnification = Some(true);
    expected_settings.color_correction = Some(CHANGED_COLOR_BLINDNESS_TYPE.into());
    expected_settings.captions_settings = Some(CHANGED_CAPTION_SETTINGS);

    let expected_info = AccessibilityInfo {
        audio_description: expected_settings.audio_description,
        screen_reader: expected_settings.screen_reader,
        color_inversion: expected_settings.color_inversion,
        enable_magnification: expected_settings.enable_magnification,
        color_correction: Some(CHANGED_COLOR_BLINDNESS_TYPE),
        captions_settings: Some(CHANGED_CAPTION_SETTINGS.into()),
    };

    // Create and fetch a store from device storage so we can read stored value for testing.
    let factory = Box::new(InMemoryStorageFactory::create());
    let store = factory.get_store::<AccessibilityInfo>();
    let accessibility_proxy = create_test_accessibility_env(factory).await;

    // Fetch the initial value.
    let settings =
        accessibility_proxy.watch().await.expect("watch completed").expect("watch successful");
    assert_eq!(settings, initial_settings);

    // Set the various settings values.
    accessibility_proxy
        .set(expected_settings.clone())
        .await
        .expect("set completed")
        .expect("set successful");

    // Verify the value we set is persisted in DeviceStorage.
    let mut store_lock = store.lock().await;
    let retrieved_struct = store_lock.get().await;
    assert_eq!(expected_info, retrieved_struct);

    // Verify the value we set is returned when watching.
    let settings =
        accessibility_proxy.watch().await.expect("watch completed").expect("watch successful");
    assert_eq!(settings, expected_settings);
}
