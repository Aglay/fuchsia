// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use failure::{Error, ResultExt};

use async_trait::async_trait;
use fidl_fuchsia_hardware_backlight::State as BacklightState;
use fidl_fuchsia_hardware_backlight::{
    DeviceMarker as BacklightMarker, DeviceProxy as BacklightProxy,
};
use fuchsia_syslog::fx_log_info;

fn open_backlight() -> Result<BacklightProxy, Error> {
    fx_log_info!("Opening backlight");
    let (proxy, server) = fidl::endpoints::create_proxy::<BacklightMarker>()
        .context("Failed to create backlight proxy")?;
    // TODO(kpt): Don't hardcode this path b/138666351
    fdio::service_connect("/dev/class/backlight/000", server.into_channel())
        .context("Failed to connect built-in service")?;
    Ok(proxy)
}

pub struct Backlight {
    proxy: BacklightProxy,
}

impl Backlight {
    pub fn new() -> Result<Backlight, Error> {
        let proxy = open_backlight()?;
        Ok(Backlight { proxy })
    }

    async fn get(&self, auto_brightness_on: bool) -> Result<u16, Error> {
        if auto_brightness_on {
            let result = self.proxy.get_state_absolute().await?;
            let backlight_info =
                result.map_err(|e| failure::format_err!("Failed to get state: {:?}", e))?;
            Ok((backlight_info.brightness) as u16)
        } else {
            let result = self.proxy.get_state_normalized().await?;
            let backlight_info =
                result.map_err(|e| failure::format_err!("Failed to get state: {:?}", e))?;
            Ok((backlight_info.brightness * 250.0) as u16)
        }
    }

    fn set(&mut self, nits: u16, auto_brightness_on: bool) -> Result<(), Error> {
        // TODO(fxb/36302): Handle error here as well, similar to get_brightness above. Might involve
        // changing this to an async function, requiring further changes in main.rs.
        if auto_brightness_on {
            let _result = self.proxy.set_state_absolute(&mut BacklightState {
                backlight_on: nits != 0,
                brightness: nits as f64,
            });
        } else {
            let _result = self.proxy.set_state_normalized(&mut BacklightState {
                backlight_on: nits != 0,
                brightness: nits as f64 / 250.0,
            });
        }
        Ok(())
    }
}

#[async_trait]
pub trait BacklightControl: Send {
    async fn get_brightness(&self, auto_brightness_on: bool) -> Result<u16, Error>;
    fn set_brightness(&mut self, value: u16, auto_brightness_on: bool) -> Result<(), Error>;
}

#[async_trait]
impl BacklightControl for Backlight {
    async fn get_brightness(&self, auto_brightness_on: bool) -> Result<u16, Error> {
        self.get(auto_brightness_on).await
    }
    fn set_brightness(&mut self, value: u16, auto_brightness_on: bool) -> Result<(), Error> {
        self.set(value, auto_brightness_on)
    }
}
