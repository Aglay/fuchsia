// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![feature(async_await)]

use failure::{bail, format_err, Error, ResultExt};
use fdio::watch_directory;
use fidl::endpoints;
use fidl_fuchsia_hardware_display::{
    ControllerEvent, ControllerMarker, ControllerProxy, ImageConfig, ImagePlane,
    ProviderSynchronousProxy,
};
use fuchsia_async::{self as fasync, DurationExt, OnSignals, TimeoutExt};
use fuchsia_zircon::{
    self as zx,
    sys::{zx_cache_policy_t::ZX_CACHE_POLICY_WRITE_COMBINING, ZX_TIME_INFINITE},
    AsHandleRef, DurationNum, Event, HandleBased, Rights, Signals, Vmo,
};
use futures::{StreamExt, TryFutureExt, TryStreamExt};
use mapped_vmo::Mapping;
use std::fs::OpenOptions;
use std::sync::Arc;

#[allow(non_camel_case_types, non_upper_case_globals)]
const ZX_PIXEL_FORMAT_NONE: u32 = 0;
#[allow(non_camel_case_types, non_upper_case_globals)]
const ZX_PIXEL_FORMAT_RGB_565: u32 = 131073;
#[allow(non_camel_case_types, non_upper_case_globals)]
const ZX_PIXEL_FORMAT_RGB_332: u32 = 65538;
#[allow(non_camel_case_types, non_upper_case_globals)]
const ZX_PIXEL_FORMAT_RGB_2220: u32 = 65539;
#[allow(non_camel_case_types, non_upper_case_globals)]
const ZX_PIXEL_FORMAT_ARGB_8888: u32 = 262148;
#[allow(non_camel_case_types, non_upper_case_globals)]
const ZX_PIXEL_FORMAT_RGB_x888: u32 = 262149;
#[allow(non_camel_case_types, non_upper_case_globals)]
const ZX_PIXEL_FORMAT_MONO_8: u32 = 65543;
#[allow(non_camel_case_types, non_upper_case_globals)]
const ZX_PIXEL_FORMAT_GRAY_8: u32 = 65543;
#[allow(non_camel_case_types, non_upper_case_globals)]
const ZX_PIXEL_FORMAT_MONO_1: u32 = 6;

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum PixelFormat {
    Argb8888,
    Gray8,
    Mono1,
    Mono8,
    Rgb2220,
    Rgb332,
    Rgb565,
    RgbX888,
    Unknown,
}

impl Default for PixelFormat {
    fn default() -> PixelFormat {
        PixelFormat::Unknown
    }
}

impl From<u32> for PixelFormat {
    fn from(pixel_format: u32) -> Self {
        #[allow(non_upper_case_globals)]
        match pixel_format {
            ZX_PIXEL_FORMAT_ARGB_8888 => PixelFormat::Argb8888,
            ZX_PIXEL_FORMAT_MONO_1 => PixelFormat::Mono1,
            ZX_PIXEL_FORMAT_MONO_8 => PixelFormat::Mono8,
            ZX_PIXEL_FORMAT_RGB_2220 => PixelFormat::Rgb2220,
            ZX_PIXEL_FORMAT_RGB_332 => PixelFormat::Rgb332,
            ZX_PIXEL_FORMAT_RGB_565 => PixelFormat::Rgb565,
            ZX_PIXEL_FORMAT_RGB_x888 => PixelFormat::RgbX888,
            // ZX_PIXEL_FORMAT_GRAY_8 is an alias for ZX_PIXEL_FORMAT_MONO_8
            ZX_PIXEL_FORMAT_NONE => PixelFormat::Unknown,
            _ => PixelFormat::Unknown,
        }
    }
}

impl Into<u32> for PixelFormat {
    fn into(self) -> u32 {
        match self {
            PixelFormat::Argb8888 => ZX_PIXEL_FORMAT_ARGB_8888,
            PixelFormat::Mono1 => ZX_PIXEL_FORMAT_MONO_1,
            PixelFormat::Mono8 => ZX_PIXEL_FORMAT_MONO_8,
            PixelFormat::Rgb2220 => ZX_PIXEL_FORMAT_RGB_2220,
            PixelFormat::Rgb332 => ZX_PIXEL_FORMAT_RGB_332,
            PixelFormat::Rgb565 => ZX_PIXEL_FORMAT_RGB_565,
            PixelFormat::RgbX888 => ZX_PIXEL_FORMAT_RGB_x888,
            PixelFormat::Gray8 => ZX_PIXEL_FORMAT_GRAY_8,
            PixelFormat::Unknown => ZX_PIXEL_FORMAT_NONE,
        }
    }
}

fn pixel_format_bytes(pixel_format: u32) -> usize {
    ((pixel_format >> 16) & 7) as usize
}

#[derive(Debug, Clone, Copy, Default)]
pub struct Config {
    pub display_id: u64,
    pub width: u32,
    pub height: u32,
    pub linear_stride_pixels: u32,
    pub format: PixelFormat,
    pub pixel_size_bytes: u32,
}

impl Config {
    pub fn linear_stride_bytes(&self) -> usize {
        self.linear_stride_pixels as usize * self.pixel_size_bytes as usize
    }
}

pub struct Frame {
    config: Config,
    pub image_id: u64,
    pub event: Event,
    signal_event_id: u64,
    pub mapping: Arc<Mapping>,
}

impl Frame {
    async fn allocate_image_vmo(framebuffer: &FrameBuffer) -> Result<Vmo, Error> {
        let (_status, vmo) =
            framebuffer.controller.allocate_vmo(framebuffer.byte_size() as u64).await?;
        Ok(vmo.unwrap())
    }

    async fn import_image_vmo(framebuffer: &FrameBuffer, image_vmo: Vmo) -> Result<u64, Error> {
        let pixel_format: u32 = framebuffer.config.format.into();
        let plane = ImagePlane {
            byte_offset: 0,
            bytes_per_row: framebuffer.config.linear_stride_bytes() as u32,
        };
        let mut image_config = ImageConfig {
            width: framebuffer.config.width,
            height: framebuffer.config.height,
            pixel_format: pixel_format as u32,
            type_: 0,
            planes: [
                plane,
                ImagePlane { byte_offset: 0, bytes_per_row: 0 },
                ImagePlane { byte_offset: 0, bytes_per_row: 0 },
                ImagePlane { byte_offset: 0, bytes_per_row: 0 },
            ],
        };

        let (_, image_id) =
            framebuffer.controller.import_vmo_image(&mut image_config, image_vmo, 0).await?;

        Ok(image_id)
    }

    pub async fn new(framebuffer: &FrameBuffer) -> Result<Frame, Error> {
        let image_vmo = Self::allocate_image_vmo(framebuffer).await?;
        image_vmo
            .set_cache_policy(ZX_CACHE_POLICY_WRITE_COMBINING)
            .unwrap_or_else(|_err| println!("set_cache_policy failed"));

        let mapping = Mapping::create_from_vmo(
            &image_vmo,
            framebuffer.byte_size(),
            zx::VmarFlags::PERM_READ
                | zx::VmarFlags::PERM_WRITE
                | zx::VmarFlags::MAP_RANGE
                | zx::VmarFlags::REQUIRE_NON_RESIZABLE,
        )
        .context("Frame::new() Mapping::create_from_vmo failed")?;

        // import image VMO
        let imported_image_vmo = image_vmo.duplicate_handle(Rights::SAME_RIGHTS)?;
        let image_id = Self::import_image_vmo(framebuffer, imported_image_vmo)
            .await
            .context("Frame::new() import_image_vmo")?;

        let my_event = Event::create()?;

        let their_event = my_event.duplicate_handle(zx::Rights::SAME_RIGHTS)?;
        let signal_event_id = my_event.get_koid()?.raw_koid();
        framebuffer
            .controller
            .import_event(Event::from_handle(their_event.into_handle()), signal_event_id)?;

        Ok(Frame {
            config: framebuffer.get_config(),
            image_id: image_id,
            event: my_event,
            signal_event_id: signal_event_id,
            mapping: Arc::new(mapping),
        })
    }

    pub fn write_pixel(&mut self, x: u32, y: u32, value: &[u8]) {
        if x < self.config.width && y < self.config.height {
            let pixel_size = self.config.pixel_size_bytes as usize;
            let offset = self.linear_stride_bytes() * y as usize + x as usize * pixel_size;
            self.write_pixel_at_offset(offset, value);
        }
    }

    pub fn write_pixel_at_offset(&mut self, offset: usize, value: &[u8]) {
        self.mapping.write_at(offset, value);
    }

    pub fn fill_rectangle(&mut self, x: u32, y: u32, width: u32, height: u32, value: &[u8]) {
        let left = x.min(self.config.width);
        let right = (left + width).min(self.config.width);
        let top = y.min(self.config.height);
        let bottom = (top + height).min(self.config.width);
        for j in top..bottom {
            for i in left..right {
                self.write_pixel(i, j, value);
            }
        }
    }

    pub fn present(
        &self,
        framebuffer: &FrameBuffer,
        sender: Option<futures::channel::mpsc::UnboundedSender<u64>>,
    ) -> Result<(), Error> {
        framebuffer
            .controller
            .set_layer_image(framebuffer.layer_id, self.image_id, 0, self.signal_event_id)
            .context("Frame::present() set_layer_image")?;
        framebuffer.controller.apply_config().context("Frame::present() apply_config")?;
        if let Some(signal_sender) = sender.as_ref() {
            let signal_sender = signal_sender.clone();
            let image_id = self.image_id;
            let local_event = self.event.duplicate_handle(zx::Rights::SAME_RIGHTS)?;
            local_event.as_handle_ref().signal(Signals::EVENT_SIGNALED, Signals::NONE)?;
            fasync::spawn_local(async move {
                let signals = OnSignals::new(&local_event, Signals::EVENT_SIGNALED);
                signals.await.expect("to wait");
                signal_sender.unbounded_send(image_id).expect("send to work");
            });
        }
        Ok(())
    }

    pub fn linear_stride_bytes(&self) -> usize {
        self.config.linear_stride_pixels as usize * self.config.pixel_size_bytes as usize
    }

    pub fn pixel_size_bytes(&self) -> usize {
        self.config.pixel_size_bytes as usize
    }
}

pub struct VSyncMessage {
    pub display_id: u64,
    pub timestamp: u64,
    pub images: Vec<u64>,
}

pub struct FrameBuffer {
    #[allow(unused)]
    display_controller: zx::Channel,
    controller: ControllerProxy,
    config: Config,
    layer_id: u64,
}

impl FrameBuffer {
    async fn create_config_from_event_stream(
        proxy: &ControllerProxy,
        stream: &mut fidl_fuchsia_hardware_display::ControllerEventStream,
    ) -> Result<Config, Error> {
        let display_id;
        let pixel_format;
        let width;
        let height;

        loop {
            let timeout = 2_i64.seconds().after_now();
            let event = stream.next().on_timeout(timeout, || None).await;
            if let Some(event) = event {
                if let Ok(ControllerEvent::DisplaysChanged { added, .. }) = event {
                    let first_added = &added[0];
                    display_id = first_added.id;
                    pixel_format = first_added.pixel_format[0];
                    width = first_added.modes[0].horizontal_resolution;
                    height = first_added.modes[0].vertical_resolution;
                    break;
                }
            } else {
                bail!("Timed out waiting for display controller to send a DisplaysChanged event");
            }
        }
        let stride = proxy.compute_linear_image_stride(width, pixel_format).await?;
        Ok(Config {
            display_id: display_id,
            width: width,
            height: height,
            linear_stride_pixels: stride,
            format: pixel_format.into(),
            pixel_size_bytes: pixel_format_bytes(pixel_format) as u32,
        })
    }

    async fn configure_layer(config: Config, proxy: &ControllerProxy) -> Result<u64, Error> {
        let (_status, layer_id) = proxy.create_layer().await?;
        let pixel_format: u32 = config.format.into();
        let plane =
            ImagePlane { byte_offset: 0, bytes_per_row: config.linear_stride_bytes() as u32 };
        let mut image_config = ImageConfig {
            width: config.width,
            height: config.height,
            pixel_format: pixel_format as u32,
            type_: 0,
            planes: [
                plane,
                ImagePlane { byte_offset: 0, bytes_per_row: 0 },
                ImagePlane { byte_offset: 0, bytes_per_row: 0 },
                ImagePlane { byte_offset: 0, bytes_per_row: 0 },
            ],
        };
        proxy.set_layer_primary_config(layer_id, &mut image_config)?;

        let mut layers = std::iter::once(layer_id);
        proxy.set_display_layers(config.display_id, &mut layers)?;
        Ok(layer_id)
    }

    pub async fn new(
        display_index: Option<usize>,
        vsync_sender: Option<futures::channel::mpsc::UnboundedSender<VSyncMessage>>,
    ) -> Result<FrameBuffer, Error> {
        let device_path = if let Some(index) = display_index {
            format!("/dev/class/display-controller/{:03}", index)
        } else {
            // If the caller did not supply a display index, we watch the
            // display-controller and use the first display that appears.
            let mut first_path = None;
            let dir = OpenOptions::new().read(true).open("/dev/class/display-controller")?;
            watch_directory(&dir, ZX_TIME_INFINITE, |_event, path| {
                first_path = Some(format!("/dev/class/display-controller/{}", path.display()));
                Err(zx::Status::STOP)
            });
            first_path.unwrap()
        };
        let file = OpenOptions::new().read(true).write(true).open(device_path)?;

        let channel = fdio::clone_channel(&file)?;
        let mut provider = ProviderSynchronousProxy::new(channel);

        let (device_client, device_server) = zx::Channel::create()?;
        let (dc_client, dc_server) = endpoints::create_endpoints::<ControllerMarker>()?;
        let status = provider.open_controller(device_server, dc_server, zx::Time::INFINITE)?;
        if status != zx::sys::ZX_OK {
            return Err(format_err!("Failed to open display controller"));
        }

        let proxy = dc_client.into_proxy()?;
        proxy.enable_vsync(true).context("enable_vsync failed")?;

        let mut stream = proxy.take_event_stream();
        let config = Self::create_config_from_event_stream(&proxy, &mut stream).await?;
        let layer_id = Self::configure_layer(config, &proxy).await?;

        if let Some(vsync_sender) = vsync_sender {
            fasync::spawn_local(
                stream
                    .map_ok(move |request| match request {
                        ControllerEvent::Vsync { display_id, timestamp, images } => {
                            vsync_sender
                                .unbounded_send(VSyncMessage { display_id, timestamp, images })
                                .unwrap_or_else(|e| eprintln!("{:?}", e));
                        }
                        _ => (),
                    })
                    .try_collect::<()>()
                    .unwrap_or_else(|e| eprintln!("view listener error: {:?}", e)),
            );
        }

        Ok(FrameBuffer { display_controller: device_client, controller: proxy, config, layer_id })
    }

    pub fn get_config(&self) -> Config {
        self.config
    }

    pub fn byte_size(&self) -> usize {
        self.config.height as usize * self.config.linear_stride_bytes()
    }
}

impl Drop for FrameBuffer {
    fn drop(&mut self) {}
}

#[cfg(test)]
mod test {
    use crate::FrameBuffer;
    use fuchsia_async::{self as fasync};

    #[test]
    fn test_async_new() -> std::result::Result<(), failure::Error> {
        let mut executor = fasync::Executor::new()?;
        let fb_future = FrameBuffer::new(None, None);
        executor.run_singlethreaded(fb_future)?;
        Ok(())
    }
}
