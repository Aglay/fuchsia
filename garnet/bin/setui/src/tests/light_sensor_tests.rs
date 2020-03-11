// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#[cfg(test)]
use {
    crate::display::LIGHT_SENSOR_SERVICE_NAME, crate::registry::device_storage::testing::*,
    crate::switchboard::base::SettingType, crate::EnvironmentBuilder, crate::Runtime,
    anyhow::format_err, fidl::endpoints::ServerEnd, fidl_fuchsia_settings::*,
    fuchsia_async as fasync, fuchsia_zircon as zx, futures::future::BoxFuture, futures::prelude::*,
};

const ENV_NAME: &str = "settings_service_light_sensor_test_environment";

const TEST_LUX_VAL: u8 = 25;
const TEST_RED_VAL: u8 = 10;
const TEST_GREEN_VAL: u8 = 9;
const TEST_BLUE_VAL: u8 = 6;

#[fuchsia_async::run_singlethreaded(test)]
async fn test_light_sensor() {
    let service_gen = |service_name: &str,
                       channel: zx::Channel|
     -> BoxFuture<'static, Result<(), anyhow::Error>> {
        if service_name != LIGHT_SENSOR_SERVICE_NAME {
            let service = String::from(service_name);
            return Box::pin(async move { Err(format_err!("{:?} unsupported!", service)) });
        }

        let stream_result =
            ServerEnd::<fidl_fuchsia_hardware_input::DeviceMarker>::new(channel).into_stream();

        if stream_result.is_err() {
            return Box::pin(async { Err(format_err!("could not connect to service")) });
        }

        let mut stream = stream_result.unwrap();

        fasync::spawn(async move {
            while let Some(request) = stream.try_next().await.unwrap() {
                if let fidl_fuchsia_hardware_input::DeviceRequest::GetReport {
                    type_: _,
                    id: _,
                    responder,
                } = request
                {
                    // Taken from actual sensor report
                    let data: [u8; 11] = [
                        1,
                        1,
                        0,
                        TEST_LUX_VAL,
                        0,
                        TEST_RED_VAL,
                        0,
                        TEST_GREEN_VAL,
                        0,
                        TEST_BLUE_VAL,
                        0,
                    ];
                    responder.send(0, &data).unwrap();
                }
            }
        });

        Box::pin(async { Ok(()) })
    };

    let env =
        EnvironmentBuilder::new(Runtime::Nested(ENV_NAME), InMemoryStorageFactory::create_handle())
            .service(Box::new(service_gen))
            .settings(&[SettingType::LightSensor])
            .spawn_and_get_nested_environment()
            .await
            .unwrap();

    let display_service = env.connect_to_service::<DisplayMarker>().unwrap();
    let data = display_service
        .watch_light_sensor(0.0)
        .await
        .expect("watch completed")
        .expect("watch successful");

    assert_eq!(data.illuminance_lux, Some(TEST_LUX_VAL.into()));
    assert_eq!(
        data.color,
        Some(fidl_fuchsia_ui_types::ColorRgb {
            red: TEST_RED_VAL.into(),
            green: TEST_GREEN_VAL.into(),
            blue: TEST_BLUE_VAL.into(),
        })
    );
}
