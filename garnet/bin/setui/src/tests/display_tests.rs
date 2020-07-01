// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#[cfg(test)]
use {
    crate::agent::restore_agent,
    crate::registry::device_storage::testing::*,
    crate::switchboard::base::{DisplayInfo, LowLightMode, SettingType},
    crate::tests::fakes::service_registry::ServiceRegistry,
    crate::tests::test_failure_utils::create_test_env_with_failures,
    crate::EnvironmentBuilder,
    anyhow::format_err,
    fidl::endpoints::{ServerEnd, ServiceMarker},
    fidl::Error::ClientChannelClosed,
    fidl_fuchsia_settings::{
        DisplayMarker, DisplayProxy, DisplaySettings, IntlMarker, LowLightMode as FidlLowLightMode,
    },
    fuchsia_async as fasync,
    fuchsia_zircon::{self as zx, Status},
    futures::future::BoxFuture,
    futures::lock::Mutex,
    futures::prelude::*,
    std::sync::Arc,
};

const ENV_NAME: &str = "settings_service_display_test_environment";
const STARTING_BRIGHTNESS: f32 = 0.5;
const CHANGED_BRIGHTNESS: f32 = 0.8;
const CONTEXT_ID: u64 = 0;

async fn setup_display_env() -> DisplayProxy {
    let service_gen = |service_name: &str,
                       channel: zx::Channel|
     -> BoxFuture<'static, Result<(), anyhow::Error>> {
        if service_name != fidl_fuchsia_ui_brightness::ControlMarker::NAME {
            return Box::pin(async { Err(format_err!("unsupported!")) });
        }

        let manager_stream_result =
            ServerEnd::<fidl_fuchsia_ui_brightness::ControlMarker>::new(channel).into_stream();

        if manager_stream_result.is_err() {
            return Box::pin(async { Err(format_err!("could not move into stream")) });
        }

        let mut manager_stream = manager_stream_result.unwrap();

        fasync::spawn(async move {
            let mut stored_brightness_value: f32 = STARTING_BRIGHTNESS;
            let mut auto_brightness_on = false;

            while let Some(req) = manager_stream.try_next().await.unwrap() {
                #[allow(unreachable_patterns)]
                match req {
                    fidl_fuchsia_ui_brightness::ControlRequest::WatchCurrentBrightness {
                        responder,
                    } => {
                        responder.send(stored_brightness_value).unwrap();
                    }
                    fidl_fuchsia_ui_brightness::ControlRequest::SetManualBrightness {
                        value,
                        control_handle: _,
                    } => {
                        stored_brightness_value = value;
                        auto_brightness_on = false;
                    }
                    fidl_fuchsia_ui_brightness::ControlRequest::SetAutoBrightness {
                        control_handle: _,
                    } => {
                        auto_brightness_on = true;
                    }
                    fidl_fuchsia_ui_brightness::ControlRequest::WatchAutoBrightness {
                        responder,
                    } => {
                        responder.send(auto_brightness_on).unwrap();
                    }
                    _ => {}
                }
            }
        });

        Box::pin(async { Ok(()) })
    };

    let env = EnvironmentBuilder::new(InMemoryStorageFactory::create())
        .service(Box::new(service_gen))
        .settings(&[SettingType::Display])
        .spawn_and_get_nested_environment(ENV_NAME)
        .await
        .unwrap();

    env.connect_to_service::<DisplayMarker>().unwrap()
}

// Creates an environment that will fail on a get request.
async fn create_display_test_env_with_failures(
    storage_factory: Arc<Mutex<InMemoryStorageFactory>>,
) -> DisplayProxy {
    create_test_env_with_failures(storage_factory, ENV_NAME, SettingType::Display)
        .await
        .connect_to_service::<DisplayMarker>()
        .unwrap()
}

/// Tests that the FIDL calls for manual brightness result in appropriate
/// commands sent to the switchboard.
#[fuchsia_async::run_until_stalled(test)]
async fn test_manual_brightness() {
    let display_proxy = setup_display_env().await;

    let settings = display_proxy.watch2().await.expect("watch completed");

    assert_eq!(settings.brightness_value, Some(STARTING_BRIGHTNESS));

    let mut display_settings = DisplaySettings::empty();
    display_settings.brightness_value = Some(CHANGED_BRIGHTNESS);
    display_proxy.set(display_settings).await.expect("set completed").expect("set successful");

    let settings = display_proxy.watch2().await.expect("watch completed");

    assert_eq!(settings.brightness_value, Some(CHANGED_BRIGHTNESS));
}

/// Tests that the FIDL calls for auto brightness result in appropriate
/// commands sent to the switchboard.
#[fuchsia_async::run_until_stalled(test)]
async fn test_auto_brightness() {
    let display_proxy = setup_display_env().await;

    let mut display_settings = DisplaySettings::empty();
    display_settings.auto_brightness = Some(true);
    display_proxy.set(display_settings).await.expect("set completed").expect("set successful");

    let settings = display_proxy.watch2().await.expect("watch completed");

    assert_eq!(settings.auto_brightness, Some(true));
}

/// Tests that the FIDL calls for light mode result in appropriate
/// commands sent to the switchboard.
#[fuchsia_async::run_until_stalled(test)]
async fn test_light_mode() {
    let display_proxy = setup_display_env().await;

    // Test that if display is enabled, it is reflected.
    let mut display_settings = DisplaySettings::empty();
    display_settings.low_light_mode = Some(FidlLowLightMode::Enable);
    display_proxy.set(display_settings).await.expect("set completed").expect("set successful");

    let settings = display_proxy.watch2().await.expect("watch completed");

    assert_eq!(settings.low_light_mode, Some(FidlLowLightMode::Enable));

    // Test that if display is disabled, it is reflected.
    let mut display_settings = DisplaySettings::empty();
    display_settings.low_light_mode = Some(FidlLowLightMode::Disable);
    display_proxy.set(display_settings).await.expect("set completed").expect("set successful");

    let settings = display_proxy.watch2().await.expect("watch completed");

    assert_eq!(settings.low_light_mode, Some(FidlLowLightMode::Disable));

    // Test that if display is disabled immediately, it is reflected.
    let mut display_settings = DisplaySettings::empty();
    display_settings.low_light_mode = Some(FidlLowLightMode::DisableImmediately);
    display_proxy.set(display_settings).await.expect("set completed").expect("set successful");

    let settings = display_proxy.watch2().await.expect("watch completed");

    assert_eq!(settings.low_light_mode, Some(FidlLowLightMode::DisableImmediately));
}

/// Makes sure that a failing display stream doesn't cause a failure for a different interface.
#[fuchsia_async::run_until_stalled(test)]
async fn test_display_restore() {
    // Ensure auto-brightness value is restored correctly.
    validate_restore(0.7, true, LowLightMode::Enable).await;

    // Ensure manual-brightness value is restored correctly.
    validate_restore(0.9, false, LowLightMode::Disable).await;
}

async fn validate_restore(
    manual_brightness: f32,
    auto_brightness: bool,
    low_light_mode: LowLightMode,
) {
    let service_registry = ServiceRegistry::create();
    let storage_factory = InMemoryStorageFactory::create();
    {
        let store = storage_factory
            .lock()
            .await
            .get_device_storage::<DisplayInfo>(StorageAccessContext::Test, CONTEXT_ID);
        let info = DisplayInfo {
            manual_brightness_value: manual_brightness,
            auto_brightness,
            low_light_mode,
        };
        assert!(store.lock().await.write(&info, false).await.is_ok());
    }

    let env = EnvironmentBuilder::new(storage_factory)
        .service(Box::new(ServiceRegistry::serve(service_registry)))
        .agents(&[restore_agent::blueprint::create()])
        .settings(&[SettingType::Display])
        .spawn_and_get_nested_environment(ENV_NAME)
        .await
        .ok();

    assert!(env.is_some());

    let display_proxy = env.unwrap().connect_to_service::<DisplayMarker>().unwrap();
    let settings = display_proxy.watch2().await.expect("watch completed");

    if auto_brightness {
        assert_eq!(settings.auto_brightness, Some(auto_brightness));
    } else {
        assert_eq!(settings.brightness_value, Some(manual_brightness));
    }
}

/// Makes sure that a failing display stream doesn't cause a failure for a different interface.
#[fuchsia_async::run_until_stalled(test)]
async fn test_display_failure() {
    let service_gen = |service_name: &str,
                       channel: zx::Channel|
     -> BoxFuture<'static, Result<(), anyhow::Error>> {
        match service_name {
            fidl_fuchsia_ui_brightness::ControlMarker::NAME => {
                // This stream is closed immediately
                let manager_stream_result =
                    ServerEnd::<fidl_fuchsia_ui_brightness::ControlMarker>::new(channel)
                        .into_stream();

                if manager_stream_result.is_err() {
                    return Box::pin(async {
                        Err(format_err!("could not move brightness channel into stream"))
                    });
                }
                return Box::pin(async { Ok(()) });
            }
            fidl_fuchsia_deprecatedtimezone::TimezoneMarker::NAME => {
                let timezone_stream_result =
                    ServerEnd::<fidl_fuchsia_deprecatedtimezone::TimezoneMarker>::new(channel)
                        .into_stream();

                if timezone_stream_result.is_err() {
                    return Box::pin(async {
                        Err(format_err!("could not move timezone channel into stream"))
                    });
                }
                let mut timezone_stream = timezone_stream_result.unwrap();
                fasync::spawn(async move {
                    while let Some(req) = timezone_stream.try_next().await.unwrap() {
                        #[allow(unreachable_patterns)]
                        match req {
                            fidl_fuchsia_deprecatedtimezone::TimezoneRequest::GetTimezoneId {
                                responder,
                            } => {
                                responder.send("PDT").unwrap();
                            }
                            _ => {}
                        }
                    }
                });
                return Box::pin(async { Ok(()) });
            }
            _ => Box::pin(async { Err(format_err!("unsupported")) }),
        }
    };

    let env = EnvironmentBuilder::new(InMemoryStorageFactory::create())
        .service(Box::new(service_gen))
        .settings(&[SettingType::Display, SettingType::Intl])
        .spawn_and_get_nested_environment(ENV_NAME)
        .await
        .unwrap();

    let display_proxy = env.connect_to_service::<DisplayMarker>().expect("connected to service");

    let _settings_value = display_proxy.watch2().await.expect("watch completed");

    let intl_service = env.connect_to_service::<IntlMarker>().unwrap();
    let _settings = intl_service.watch2().await.expect("watch completed");
}

#[fuchsia_async::run_until_stalled(test)]
async fn test_channel_failure_watch2() {
    let display_proxy =
        create_display_test_env_with_failures(InMemoryStorageFactory::create()).await;
    let result = display_proxy.watch2().await;
    assert!(result.is_err());
    assert_eq!(
        ClientChannelClosed(Status::INTERNAL).to_string(),
        result.err().unwrap().to_string()
    );
}
