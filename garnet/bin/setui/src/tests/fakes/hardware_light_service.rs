// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::tests::fakes::base::Service;
use anyhow::{format_err, Error};
use fidl::endpoints::ServerEnd;
use fidl::endpoints::ServiceMarker;
use fidl_fuchsia_hardware_light::{Info, LightMarker, LightRequest, Rgb};
use fuchsia_async as fasync;
use fuchsia_zircon::Channel;
use futures::lock::Mutex;
use futures::TryStreamExt;
use std::collections::HashMap;
use std::sync::Arc;

/// An implementation of fuchsia.hardware.light for testing use.
pub struct HardwareLightService {
    light_info: Arc<Mutex<HashMap<u32, Info>>>,
    simple_values: Arc<Mutex<HashMap<u32, bool>>>,
    brightness_values: Arc<Mutex<HashMap<u32, u8>>>,
    rgb_values: Arc<Mutex<HashMap<u32, Rgb>>>,
}

/// Allow dead code since this is just a fake for testing.
#[allow(dead_code)]
impl HardwareLightService {
    pub fn new() -> Self {
        Self {
            light_info: Arc::new(Mutex::new(HashMap::new())),
            simple_values: Arc::new(Mutex::new(HashMap::new())),
            brightness_values: Arc::new(Mutex::new(HashMap::new())),
            rgb_values: Arc::new(Mutex::new(HashMap::new())),
        }
    }

    pub async fn insert_light_info(&self, index: u32, value: Info) {
        self.light_info.lock().await.insert(index, value);
    }

    pub async fn insert_simple_value(&self, index: u32, value: bool) {
        self.simple_values.lock().await.insert(index, value);
    }

    pub async fn insert_brightness_value(&self, index: u32, value: u8) {
        self.brightness_values.lock().await.insert(index, value);
    }

    pub async fn insert_rgb_value(&self, index: u32, value: Rgb) {
        self.rgb_values.lock().await.insert(index, value);
    }
}

impl Service for HardwareLightService {
    fn can_handle_service(&self, service_name: &str) -> bool {
        return service_name == LightMarker::NAME;
    }

    fn process_stream(&self, service_name: &str, channel: Channel) -> Result<(), Error> {
        if !self.can_handle_service(service_name) {
            return Err(format_err!("unsupported"));
        }

        let mut request_stream = ServerEnd::<LightMarker>::new(channel).into_stream()?;

        let light_info = self.light_info.clone();
        let simple_values = self.simple_values.clone();
        let brightness_values = self.brightness_values.clone();
        let rgb_values = self.rgb_values.clone();
        fasync::spawn(async move {
            while let Some(req) = request_stream.try_next().await.unwrap() {
                match req {
                    LightRequest::GetNumLights { responder } => responder
                        .send(light_info.lock().await.len() as u32)
                        .expect("get num lights"),
                    LightRequest::GetInfo { index, responder } => responder
                        .send(&mut Ok(light_info
                            .lock()
                            .await
                            .get(&index)
                            .expect("unknown light")
                            .clone()))
                        .expect("get num lights"),
                    LightRequest::GetCurrentBrightnessValue { index, responder } => responder
                        .send(&mut Ok(brightness_values
                            .lock()
                            .await
                            .get(&index)
                            .expect("unknown light")
                            .clone()))
                        .expect("get brightness value"),
                    LightRequest::GetCurrentSimpleValue { index, responder } => responder
                        .send(&mut Ok(simple_values
                            .lock()
                            .await
                            .get(&index)
                            .expect("unknown light")
                            .clone()))
                        .expect("get simple value"),
                    LightRequest::GetCurrentRgbValue { index, responder } => responder
                        .send(&mut Ok(rgb_values
                            .lock()
                            .await
                            .get(&index)
                            .expect("unknown light")
                            .clone()))
                        .expect("get rgb value"),
                    _ => {}
                }
            }
        });

        Ok(())
    }
}
