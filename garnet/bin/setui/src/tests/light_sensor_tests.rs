// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#[cfg(test)]
use {
    crate::create_fidl_service, crate::display::LIGHT_SENSOR_SERVICE_NAME,
    crate::registry::device_storage::testing::*, crate::registry::service_context::ServiceContext,
    crate::switchboard::base::SettingType, failure::format_err, fidl::endpoints::ServerEnd,
    fidl_fuchsia_settings::*, fuchsia_async as fasync, fuchsia_component::server::ServiceFs,
    fuchsia_zircon as zx, futures::prelude::*, parking_lot::RwLock, std::sync::Arc,
};

const ENV_NAME: &str = "settings_service_light_sensor_test_environment";

#[fuchsia_async::run_singlethreaded(test)]
async fn test_light_sensor() {
    let service_gen = |service_name: &str, channel: zx::Channel| {
        if service_name != LIGHT_SENSOR_SERVICE_NAME {
            return Err(format_err!("{:?} unsupported!", service_name));
        }

        let mut stream =
            ServerEnd::<fidl_fuchsia_hardware_input::DeviceMarker>::new(channel).into_stream()?;

        fasync::spawn(async move {
            while let Some(request) = stream.try_next().await.unwrap() {
                if let fidl_fuchsia_hardware_input::DeviceRequest::GetReport {
                    type_: _,
                    id: _,
                    responder,
                } = request
                {
                    // Taken from actual sensor report
                    let data: [u8; 11] = [1, 1, 0, 25, 0, 10, 0, 9, 0, 6, 0];
                    responder.send(0, &mut data.iter().cloned()).unwrap();
                }
            }
        });
        Ok(())
    };

    let mut fs = ServiceFs::new();

    create_fidl_service(
        fs.root_dir(),
        [SettingType::LightSensor].iter().cloned().collect(),
        Arc::new(RwLock::new(ServiceContext::new(Some(Box::new(service_gen))))),
        Box::new(InMemoryStorageFactory::create()),
    );

    let env = fs.create_salted_nested_environment(ENV_NAME).unwrap();
    fasync::spawn(fs.collect());

    let display_service = env.connect_to_service::<DisplayMarker>().unwrap();
    let data = display_service
        .watch_light_sensor(0.0)
        .await
        .expect("watch completed")
        .expect("watch successful");

    assert_eq!(data.illuminance_lux, Some(25.0));
}
