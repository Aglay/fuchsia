// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
use crate::registry::setting_handler::persist::{
    controller as data_controller, write, ClientProxy,
};
use crate::registry::setting_handler::{controller, ControllerError};
use async_trait::async_trait;
use {
    crate::audio::{default_audio_info, StreamVolumeControl},
    crate::input::monitor_media_buttons,
    crate::registry::base::State,
    crate::switchboard::base::*,
    anyhow::Error,
    fidl_fuchsia_ui_input::MediaButtonsEvent,
    fuchsia_async as fasync,
    fuchsia_syslog::fx_log_err,
    futures::lock::Mutex,
    futures::StreamExt,
    std::collections::HashMap,
    std::sync::Arc,
};

fn get_streams_array_from_map(
    stream_map: &HashMap<AudioStreamType, StreamVolumeControl>,
) -> [AudioStream; 5] {
    let mut streams: [AudioStream; 5] = default_audio_info().streams;
    for i in 0..streams.len() {
        if let Some(volume_control) = stream_map.get(&streams[i].stream_type) {
            streams[i] = volume_control.stored_stream.clone();
        }
    }
    streams
}

type InputMonitorHandle = Arc<Mutex<InputMonitor>>;
type MediaButtonEventSender = futures::channel::mpsc::UnboundedSender<MediaButtonsEvent>;

struct InputMonitor {
    client: ClientProxy<AudioInfo>,
    service_connected: bool,
    input_tx: MediaButtonEventSender,
    mic_mute_state: bool,
    volume_button_event: i8,
}

impl InputMonitor {
    fn create(client: ClientProxy<AudioInfo>) -> InputMonitorHandle {
        let (input_tx, mut input_rx) = futures::channel::mpsc::unbounded::<MediaButtonsEvent>();
        let default_audio_settings = default_audio_info();
        let monitor_handle = Arc::new(Mutex::new(Self {
            client: client,
            service_connected: false,
            input_tx: input_tx,
            mic_mute_state: default_audio_settings.input.mic_mute,
            volume_button_event: 0,
        }));

        let monitor_handle_clone = monitor_handle.clone();
        fasync::spawn(async move {
            monitor_handle_clone.lock().await.ensure_monitor().await;

            while let Some(event) = input_rx.next().await {
                monitor_handle_clone.lock().await.process_event(event).await;
            }
        });

        monitor_handle
    }

    pub fn get_mute_state(&self) -> bool {
        self.mic_mute_state
    }

    async fn process_event(&mut self, event: MediaButtonsEvent) {
        if let Some(mic_mute) = event.mic_mute {
            if self.mic_mute_state != mic_mute {
                self.mic_mute_state = mic_mute;
                self.client.notify().await;
            }
        }
        if let Some(volume) = event.volume {
            self.volume_button_event = volume;
            self.client.notify().await;
        }
    }

    async fn ensure_monitor(&mut self) {
        if self.service_connected {
            return;
        }

        self.service_connected = monitor_media_buttons(
            self.client.get_service_context().await.clone(),
            self.input_tx.clone(),
        )
        .await
        .is_ok();
    }
}

type VolumeControllerHandle = Arc<Mutex<VolumeController>>;

pub struct VolumeController {
    client: ClientProxy<AudioInfo>,
    audio_service_connected: bool,
    stream_volume_controls: HashMap<AudioStreamType, StreamVolumeControl>,
    input_monitor: InputMonitorHandle,
    changed_streams: Option<Vec<AudioStream>>,
}

impl VolumeController {
    async fn create(client: ClientProxy<AudioInfo>) -> VolumeControllerHandle {
        let handle = Arc::new(Mutex::new(Self {
            client: client.clone(),
            stream_volume_controls: HashMap::new(),
            audio_service_connected: false,
            input_monitor: InputMonitor::create(client.clone()),
            changed_streams: None,
        }));

        handle.lock().await.check_and_bind_volume_controls().await.ok();

        handle
    }

    async fn restore(&mut self) {
        let audio_info = self.client.read().await;
        let stored_streams = audio_info.streams.iter().cloned().collect();
        self.update_volume_stream(&stored_streams).await;
    }

    async fn get_info(&mut self) -> Result<AudioInfo, SwitchboardError> {
        let mut audio_info = self.client.read().await;
        self.input_monitor.lock().await.ensure_monitor().await;
        if self.check_and_bind_volume_controls().await.is_err() {
            // TODO(fxb/49663): This should return an error instead of returning
            // default data.
            return Ok(default_audio_info());
        };

        audio_info.input =
            AudioInputInfo { mic_mute: self.input_monitor.lock().await.get_mute_state() };
        audio_info.changed_streams = self.changed_streams.clone();
        Ok(audio_info)
    }

    async fn set_volume(&mut self, volume: Vec<AudioStream>) -> SettingResponseResult {
        let get_result = self.get_info().await;

        if let Err(e) = get_result {
            return Err(e);
        }

        self.update_volume_stream(&volume).await;
        self.changed_streams = Some(volume);

        Ok(None)
    }

    async fn update_volume_stream(&mut self, new_streams: &Vec<AudioStream>) {
        for stream in new_streams {
            if let Some(volume_control) = self.stream_volume_controls.get_mut(&stream.stream_type) {
                volume_control.set_volume(stream.clone()).await;
            }
        }

        let mut stored_value = self.client.read().await;
        stored_value.streams = get_streams_array_from_map(&self.stream_volume_controls);
        write(&self.client, stored_value, false).await.ok();
    }

    async fn check_and_bind_volume_controls(&mut self) -> Result<(), Error> {
        if self.audio_service_connected {
            return Ok(());
        }

        let service_result = self
            .client
            .get_service_context()
            .await
            .lock()
            .await
            .connect::<fidl_fuchsia_media::AudioCoreMarker>()
            .await;

        let audio_service = match service_result {
            Ok(service) => {
                self.audio_service_connected = true;
                service
            }
            Err(err) => {
                fx_log_err!("failed to connect to audio core, {}", err);
                return Err(err);
            }
        };

        for stream in default_audio_info().streams.iter() {
            self.stream_volume_controls.insert(
                stream.stream_type.clone(),
                StreamVolumeControl::create(&audio_service, stream.clone()),
            );
        }

        Ok(())
    }
}

pub struct AudioController {
    volume: VolumeControllerHandle,
}

#[async_trait]
impl data_controller::Create<AudioInfo> for AudioController {
    /// Creates the controller
    async fn create(client: ClientProxy<AudioInfo>) -> Result<Self, ControllerError> {
        Ok(AudioController { volume: VolumeController::create(client).await })
    }
}

#[async_trait]
impl controller::Handle for AudioController {
    async fn handle(&self, request: SettingRequest) -> Option<SettingResponseResult> {
        #[allow(unreachable_patterns)]
        match request {
            SettingRequest::Restore => {
                self.volume.lock().await.restore().await;

                Some(Ok(None))
            }
            SettingRequest::SetVolume(volume) => {
                Some(self.volume.lock().await.set_volume(volume).await)
            }
            SettingRequest::Get => Some(match self.volume.lock().await.get_info().await {
                Ok(info) => Ok(Some(SettingResponse::Audio(info))),
                Err(e) => Err(e),
            }),
            _ => None,
        }
    }

    async fn change_state(&mut self, _: State) {}
}
