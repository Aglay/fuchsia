// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#[cfg(test)]
use {
    crate::fidl_clone::FIDLClone,
    crate::registry::device_storage::testing::*,
    crate::switchboard::accessibility_types::{AccessibilityInfo, ColorBlindnessType},
    crate::switchboard::base::{SettingRequest, SettingType, SwitchboardError},
    crate::tests::fakes::base::create_setting_handler,
    crate::EnvironmentBuilder,
    fidl::Error::ClientChannelClosed,
    fidl_fuchsia_settings::*,
    fidl_fuchsia_ui_types::ColorRgba,
    fuchsia_zircon::Status,
    futures::lock::Mutex,
    std::sync::Arc,
};

const ENV_NAME: &str = "settings_service_accessibility_test_environment";

async fn create_test_accessibility_env(
    storage_factory: Arc<Mutex<InMemoryStorageFactory>>,
) -> AccessibilityProxy {
    EnvironmentBuilder::new(storage_factory)
        .settings(&[SettingType::Accessibility])
        .spawn_and_get_nested_environment(ENV_NAME)
        .await
        .unwrap()
        .connect_to_service::<AccessibilityMarker>()
        .unwrap()
}

// Creates an environment that will fail on a get request.
async fn create_test_env_with_failures(
    storage_factory: Arc<Mutex<InMemoryStorageFactory>>,
) -> AccessibilityProxy {
    EnvironmentBuilder::new(storage_factory)
        .handler(
            SettingType::Accessibility,
            create_setting_handler(Box::new(move |request| {
                if request == SettingRequest::Get {
                    Box::pin(async move {
                        Err(SwitchboardError::UnhandledType {
                            setting_type: SettingType::Accessibility,
                        })
                    })
                } else {
                    Box::pin(async { Ok(None) })
                }
            })),
        )
        .settings(&[SettingType::Accessibility])
        .spawn_and_get_nested_environment(ENV_NAME)
        .await
        .unwrap()
        .connect_to_service::<AccessibilityMarker>()
        .unwrap()
}

#[fuchsia_async::run_singlethreaded(test)]
async fn test_accessibility_set_all() {
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
    let factory = InMemoryStorageFactory::create();
    let store =
        factory.lock().await.get_device_storage::<AccessibilityInfo>(StorageAccessContext::Test);
    let accessibility_proxy = create_test_accessibility_env(factory).await;

    // Fetch the initial value.
    let settings = accessibility_proxy.watch2().await.expect("watch completed");
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
    let settings = accessibility_proxy.watch2().await.expect("watch completed");
    assert_eq!(settings, expected_settings);
}

#[fuchsia_async::run_singlethreaded(test)]
async fn test_accessibility_set_captions() {
    const CHANGED_FONT_STYLE: CaptionFontStyle = CaptionFontStyle {
        family: Some(CaptionFontFamily::Casual),
        color: None,
        relative_size: Some(1.0),
        char_edge_style: None,
    };
    const EXPECTED_CAPTIONS_SETTINGS: CaptionsSettings = CaptionsSettings {
        for_media: Some(true),
        for_tts: None,
        font_style: Some(CHANGED_FONT_STYLE),
        window_color: Some(ColorRgba { red: 238.0, green: 23.0, blue: 128.0, alpha: 255.0 }),
        background_color: None,
    };

    let mut expected_settings = AccessibilitySettings::empty();
    expected_settings.captions_settings = Some(EXPECTED_CAPTIONS_SETTINGS);

    let expected_info = AccessibilityInfo {
        audio_description: None,
        screen_reader: None,
        color_inversion: None,
        enable_magnification: None,
        color_correction: None,
        captions_settings: Some(EXPECTED_CAPTIONS_SETTINGS.into()),
    };

    // Create and fetch a store from device storage so we can read stored value for testing.
    let factory = InMemoryStorageFactory::create();
    let store =
        factory.lock().await.get_device_storage::<AccessibilityInfo>(StorageAccessContext::Test);
    let accessibility_proxy = create_test_accessibility_env(factory).await;

    // Set for_media and window_color in the top-level CaptionsSettings.
    let mut first_set = AccessibilitySettings::empty();
    first_set.captions_settings = Some(CaptionsSettings {
        for_media: Some(false),
        for_tts: None,
        font_style: None,
        window_color: EXPECTED_CAPTIONS_SETTINGS.window_color,
        background_color: None,
    });
    accessibility_proxy
        .set(first_set.clone())
        .await
        .expect("set completed")
        .expect("set successful");

    // Set FontStyle and overwrite for_media.
    let mut second_set = AccessibilitySettings::empty();
    second_set.captions_settings = Some(CaptionsSettings {
        for_media: EXPECTED_CAPTIONS_SETTINGS.for_media,
        for_tts: None,
        font_style: EXPECTED_CAPTIONS_SETTINGS.font_style,
        window_color: None,
        background_color: None,
    });
    accessibility_proxy
        .set(second_set.clone())
        .await
        .expect("set completed")
        .expect("set successful");

    // Verify the value we set is persisted in DeviceStorage.
    let mut store_lock = store.lock().await;
    let retrieved_struct = store_lock.get().await;
    assert_eq!(expected_info, retrieved_struct);

    // Verify the value we set is returned when watching.
    let settings = accessibility_proxy.watch2().await.expect("watch completed");
    assert_eq!(settings, expected_settings);
}

#[fuchsia_async::run_singlethreaded(test)]
async fn test_channel_failure_watch() {
    let accessibility_proxy = create_test_env_with_failures(InMemoryStorageFactory::create()).await;
    let result = accessibility_proxy.watch().await.ok();
    assert_eq!(result, Some(Err(Error::Failed)));
}

#[fuchsia_async::run_singlethreaded(test)]
async fn test_channel_failure_watch2() {
    let accessibility_proxy = create_test_env_with_failures(InMemoryStorageFactory::create()).await;
    let result = accessibility_proxy.watch2().await;
    assert!(result.is_err());
    assert_eq!(
        ClientChannelClosed(Status::INTERNAL).to_string(),
        result.err().unwrap().to_string()
    );
}

#[fuchsia_async::run_singlethreaded(test)]
async fn test_simultaneous_watch() {
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

    let mut expected_settings = AccessibilitySettings::empty();
    expected_settings.audio_description = Some(true);
    expected_settings.screen_reader = Some(true);
    expected_settings.color_inversion = Some(true);
    expected_settings.enable_magnification = Some(true);
    expected_settings.color_correction = Some(CHANGED_COLOR_BLINDNESS_TYPE.into());
    expected_settings.captions_settings = Some(CHANGED_CAPTION_SETTINGS);

    let accessibility_proxy = create_test_accessibility_env(InMemoryStorageFactory::create()).await;

    // Set the various settings values.
    accessibility_proxy
        .set(expected_settings.clone())
        .await
        .expect("set completed")
        .expect("set successful");

    // Watch for the result with both watches.
    let result = accessibility_proxy.watch().await.ok();
    let result2 = accessibility_proxy.watch2().await.ok();

    assert_eq!(result, Some(Ok(expected_settings.clone())));
    assert_eq!(result2, Some(expected_settings.clone()));
}
