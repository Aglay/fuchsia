// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#[cfg(test)]
use {
    crate::registry::device_storage::testing::*,
    crate::registry::device_storage::DeviceStorageFactory,
    crate::switchboard::base::SettingType,
    crate::EnvironmentBuilder,
    crate::Runtime,
    anyhow::format_err,
    fidl::endpoints::{ServerEnd, ServiceMarker},
    fidl_fuchsia_settings::*,
    fuchsia_async as fasync, fuchsia_zircon as zx,
    futures::lock::Mutex,
    futures::prelude::*,
    std::sync::Arc,
};

use crate::fidl_clone::FIDLClone;
use crate::switchboard::intl_types::IntlInfo;

const ENV_NAME: &str = "settings_service_intl_test_environment";

async fn create_test_intl_env(storage_factory: Arc<Mutex<InMemoryStorageFactory>>) -> IntlProxy {
    let service_gen = |service_name: &str, channel: zx::Channel| {
        if service_name != fidl_fuchsia_deprecatedtimezone::TimezoneMarker::NAME {
            return Err(format_err!("unsupported!"));
        }

        let mut timezone_stream =
            ServerEnd::<fidl_fuchsia_deprecatedtimezone::TimezoneMarker>::new(channel)
                .into_stream()?;

        fasync::spawn(async move {
            while let Some(req) = timezone_stream.try_next().await.unwrap() {
                #[allow(unreachable_patterns)]
                match req {
                    fidl_fuchsia_deprecatedtimezone::TimezoneRequest::GetTimezoneId {
                        responder,
                    } => {
                        responder.send("PDT").unwrap();
                    }
                    fidl_fuchsia_deprecatedtimezone::TimezoneRequest::SetTimezone {
                        timezone_id: _,
                        responder,
                    } => {
                        responder.send(true).unwrap();
                    }
                    _ => {}
                }
            }
        });
        Ok(())
    };

    let env = EnvironmentBuilder::new(Runtime::Nested(ENV_NAME), storage_factory)
        .service(Box::new(service_gen))
        .settings(&[SettingType::Intl])
        .spawn_and_get_nested_environment()
        .await
        .unwrap();

    env.connect_to_service::<IntlMarker>().unwrap()
}

#[fuchsia_async::run_singlethreaded(test)]
async fn test_intl_e2e() {
    // Create and fetch a store from device storage so we can read stored value for testing.
    let factory = InMemoryStorageFactory::create_handle();
    let store = factory.lock().await.get_store::<IntlInfo>();
    let intl_service = create_test_intl_env(factory).await;

    // Check if the initial value is correct.
    let settings = intl_service.watch().await.expect("watch completed").expect("watch successful");
    assert_eq!(
        settings.time_zone_id,
        Some(fidl_fuchsia_intl::TimeZoneId { id: "UTC".to_string() })
    );
    assert_eq!(
        settings.locales,
        Some(vec![fidl_fuchsia_intl::LocaleId { id: "en-US".to_string() }])
    );
    assert_eq!(settings.temperature_unit, Some(fidl_fuchsia_intl::TemperatureUnit::Celsius));
    assert_eq!(settings.hour_cycle, Some(fidl_fuchsia_settings::HourCycle::H12));

    // Set new values.
    let intl_settings = fidl_fuchsia_settings::IntlSettings {
        locales: Some(vec![fidl_fuchsia_intl::LocaleId { id: "blah".into() }]),
        temperature_unit: Some(fidl_fuchsia_intl::TemperatureUnit::Celsius),
        time_zone_id: Some(fidl_fuchsia_intl::TimeZoneId { id: "GMT".to_string() }),
        hour_cycle: Some(fidl_fuchsia_settings::HourCycle::H24),
    };
    intl_service.set(intl_settings.clone()).await.expect("set completed").expect("set successful");

    // Verify the values we set are returned when watching.
    let settings = intl_service.watch().await.expect("watch completed").expect("watch successful");
    assert_eq!(settings, intl_settings.clone());

    // Verify the value we set is persisted in DeviceStorage.
    let mut store_lock = store.lock().await;
    let retrieved_struct = store_lock.get().await;
    assert_eq!(retrieved_struct, intl_settings.clone().into());
}

#[fuchsia_async::run_singlethreaded(test)]
async fn test_intl_e2e_set_twice() {
    // Create and fetch a store from device storage so we can read stored value for testing.
    let factory = InMemoryStorageFactory::create_handle();
    let store = factory.lock().await.get_store::<IntlInfo>();
    let intl_service = create_test_intl_env(factory).await;

    // Initial value is not None.
    let settings = intl_service.watch().await.expect("watch completed").expect("watch successful");
    assert_eq!(
        settings.time_zone_id,
        Some(fidl_fuchsia_intl::TimeZoneId { id: "UTC".to_string() })
    );
    assert_eq!(
        settings.locales,
        Some(vec![fidl_fuchsia_intl::LocaleId { id: "en-US".to_string() }])
    );
    assert_eq!(settings.temperature_unit, Some(fidl_fuchsia_intl::TemperatureUnit::Celsius));
    assert_eq!(settings.hour_cycle, Some(fidl_fuchsia_settings::HourCycle::H12));

    // Set new values.
    let mut intl_settings = fidl_fuchsia_settings::IntlSettings::empty();
    let updated_timezone = "GMT";
    intl_settings.time_zone_id =
        Some(fidl_fuchsia_intl::TimeZoneId { id: updated_timezone.to_string() });
    intl_settings.hour_cycle = Some(fidl_fuchsia_settings::HourCycle::H24);
    intl_service.set(intl_settings).await.expect("set completed").expect("set successful");

    // Try to set to a new value: this second set should succeed too.
    let mut intl_settings = fidl_fuchsia_settings::IntlSettings::empty();
    let updated_timezone = "PST";
    intl_settings.time_zone_id =
        Some(fidl_fuchsia_intl::TimeZoneId { id: updated_timezone.to_string() });
    intl_service.set(intl_settings).await.expect("set completed").expect("repeated set successful");

    // Verify the value we set is persisted in DeviceStorage.
    let mut store_lock = store.lock().await;
    let retrieved_struct = store_lock.get().await;
    assert_eq!(retrieved_struct.time_zone_id.unwrap(), updated_timezone);
}

#[fuchsia_async::run_singlethreaded(test)]
async fn test_intl_e2e_idempotent_set() {
    // Create and fetch a store from device storage so we can read stored value for testing.
    let factory = InMemoryStorageFactory::create_handle();
    let store = factory.lock().await.get_store::<IntlInfo>();
    let intl_service = create_test_intl_env(factory).await;

    // Check if the initial value is correct.
    let settings = intl_service.watch().await.expect("watch completed").expect("watch successful");
    assert_eq!(
        settings.time_zone_id,
        Some(fidl_fuchsia_intl::TimeZoneId { id: "UTC".to_string() })
    );
    assert_eq!(
        settings.locales,
        Some(vec![fidl_fuchsia_intl::LocaleId { id: "en-US".to_string() }])
    );
    assert_eq!(settings.temperature_unit, Some(fidl_fuchsia_intl::TemperatureUnit::Celsius));
    assert_eq!(settings.hour_cycle, Some(fidl_fuchsia_settings::HourCycle::H12));

    // Set new values.
    let mut intl_settings = fidl_fuchsia_settings::IntlSettings::empty();
    let updated_timezone = "GMT";
    intl_settings.time_zone_id =
        Some(fidl_fuchsia_intl::TimeZoneId { id: updated_timezone.to_string() });
    intl_service.set(intl_settings).await.expect("set completed").expect("set successful");

    // Try to set again to the same value: this second set should succeed.
    let mut intl_settings = fidl_fuchsia_settings::IntlSettings::empty();
    intl_settings.time_zone_id =
        Some(fidl_fuchsia_intl::TimeZoneId { id: updated_timezone.to_string() });
    intl_service.set(intl_settings).await.expect("set completed").expect("repeated set successful");

    // Verify the value we set is persisted in DeviceStorage.
    let mut store_lock = store.lock().await;
    let retrieved_struct = store_lock.get().await;
    assert_eq!(retrieved_struct.time_zone_id.unwrap(), updated_timezone);
}

#[fuchsia_async::run_singlethreaded(test)]
async fn test_intl_invalid_timezone() {
    const INITIAL_TIME_ZONE: &'static str = "GMT";

    let factory = InMemoryStorageFactory::create_handle();
    let intl_service = create_test_intl_env(factory).await;

    // Set a real value.
    let mut intl_settings = fidl_fuchsia_settings::IntlSettings::empty();
    intl_settings.time_zone_id =
        Some(fidl_fuchsia_intl::TimeZoneId { id: INITIAL_TIME_ZONE.to_string() });
    intl_service.set(intl_settings).await.expect("set completed").expect("set successful");

    // Set with an invalid timezone value.
    let mut intl_settings = fidl_fuchsia_settings::IntlSettings::empty();
    let updated_timezone = "not_a_real_time_zone";
    intl_settings.time_zone_id =
        Some(fidl_fuchsia_intl::TimeZoneId { id: updated_timezone.to_string() });
    intl_service.set(intl_settings).await.expect("set completed").expect_err("invalid");

    // Verify the returned when watching hasn't changed.
    let settings = intl_service.watch().await.expect("watch completed").expect("watch successful");
    assert_eq!(
        settings.time_zone_id,
        Some(fidl_fuchsia_intl::TimeZoneId { id: INITIAL_TIME_ZONE.to_string() })
    );
}
