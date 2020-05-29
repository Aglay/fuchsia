// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::agent::earcons::agent::CommonEarconsParams;
use crate::agent::earcons::sound_ids::{VOLUME_CHANGED_SOUND_ID, VOLUME_MAX_SOUND_ID};
use crate::agent::earcons::utils::{connect_to_sound_player, play_sound};
use crate::input::monitor_media_buttons;
use crate::switchboard::base::{
    AudioInfo, AudioStreamType, ListenSession, SettingRequest, SettingResponse, SettingType,
    SwitchboardClient,
};

use anyhow::Error;
use fidl::endpoints::create_request_stream;
use fidl_fuchsia_media::{
    AudioRenderUsage,
    Usage::RenderUsage,
    UsageReporterMarker,
    UsageState::{Ducked, Muted},
    UsageWatcherRequest,
    UsageWatcherRequest::OnStateChanged,
};
use fidl_fuchsia_ui_input::MediaButtonsEvent;
use fuchsia_async as fasync;
use fuchsia_syslog::{fx_log_debug, fx_log_err};
use futures::StreamExt;
use std::sync::Arc;

/// The `VolumeChangeHandler` takes care of the earcons functionality on volume change.
pub struct VolumeChangeHandler {
    _listen_session: Box<dyn ListenSession + Send + Sync + 'static>,
    priority_stream_playing: bool,
    common_earcons_params: CommonEarconsParams,
    last_media_user_volume: Option<f32>,
    volume_button_event: i8,
    switchboard_client: SwitchboardClient,
}

/// The maximum volume level.
const MAX_VOLUME: f32 = 1.0;

/// The file path for the earcon to be played for max sound level.
const VOLUME_MAX_FILE_PATH: &str = "volume-max.wav";

/// The file path for the earcon to be played for volume changes below max volume level.
const VOLUME_CHANGED_FILE_PATH: &str = "volume-changed.wav";

impl VolumeChangeHandler {
    pub async fn create(
        params: CommonEarconsParams,
        client: SwitchboardClient,
    ) -> Result<(), Error> {
        // Listen to button presses.
        let (input_tx, mut input_rx) = futures::channel::mpsc::unbounded::<MediaButtonsEvent>();
        monitor_media_buttons(params.service_context.clone(), input_tx).await?;

        // Listen to audio change events.
        let (audio_tx, mut audio_rx) = futures::channel::mpsc::unbounded::<SettingType>();
        let session = client
            .clone()
            .listen(
                SettingType::Audio,
                Arc::new(move |setting| {
                    audio_tx.unbounded_send(setting).ok();
                }),
            )
            .await?;

        let (volume_tx, mut volume_rx) = futures::channel::mpsc::unbounded::<SettingResponse>();

        let usage_reporter_proxy =
            params.service_context.lock().await.connect::<UsageReporterMarker>().await?;

        // Create channel for usage reporter watch results.
        let (usage_tx, mut usage_rx) = create_request_stream()?;

        // Watch for changes in usage.
        usage_reporter_proxy.watch(&mut RenderUsage(AudioRenderUsage::Background), usage_tx)?;

        fasync::spawn(async move {
            let mut handler = Self {
                _listen_session: session,
                common_earcons_params: params,
                last_media_user_volume: Some(1.0),
                volume_button_event: 0,
                priority_stream_playing: false,
                switchboard_client: client,
            };

            loop {
                futures::select! {
                    event = input_rx.next() => {
                        if let Some(event) = event {
                            handler.on_button_event(event).await;
                        }
                    }
                    changed_setting = audio_rx.next() => {
                        if let Some(setting) = changed_setting {
                            handler.on_changed_setting(setting, volume_tx.clone()).await;
                        }
                    }
                    volume_response = volume_rx.next() => {
                        if let Some(SettingResponse::Audio(audio_info)) = volume_response {
                            handler.on_audio_info(audio_info).await;
                        }
                    }
                    background_usage = usage_rx.next() => {
                        if let Some(Ok(request)) = background_usage {
                            handler.on_background_usage(request);
                        }

                    }
                }
            }
        });

        Ok(())
    }

    /// Called when a new media button input event is available from the
    /// listener. Stores the last event.
    async fn on_button_event(&mut self, event: MediaButtonsEvent) {
        if let Some(volume) = event.volume {
            self.volume_button_event = volume;
        }
    }

    /// Called when a setting `VolumeChangeHandler` has registered as a listener
    /// for indicates there is a new value. Requests the updated value from
    /// the `Switchboard`.
    async fn on_changed_setting(
        &mut self,
        setting_type: SettingType,
        response_sender: futures::channel::mpsc::UnboundedSender<SettingResponse>,
    ) {
        match self.switchboard_client.request(setting_type, SettingRequest::Get).await {
            Ok(response_rx) => {
                fasync::spawn(async move {
                    match response_rx.await {
                        Ok(Ok(Some(response))) => {
                            response_sender.unbounded_send(response).ok();
                        }
                        _ => {
                            fx_log_err!("[earcons_agent] Failed to extract volume state response from switchboard");
                        }
                    }
                });
            }
            Err(err) => {
                fx_log_err!(
                    "[earcons_agent] Failed to get volume state from switchboard: {:?}",
                    err
                );
            }
        }
    }

    /// Invoked when a new `AudioInfo` is retrieved. Determines whether an
    /// earcon should be played and plays sound if necessary.
    async fn on_audio_info(&mut self, audio_info: AudioInfo) {
        let changed_streams = match audio_info.changed_streams {
            Some(streams) => streams,
            None => Vec::new(),
        };

        let new_media_user_volume: Option<f32> =
            match changed_streams.iter().find(|&&x| x.stream_type == AudioStreamType::Media) {
                Some(stream) => Some(stream.user_volume_level),
                None => None,
            };
        let volume_up_max_pressed =
            new_media_user_volume == Some(MAX_VOLUME) && self.volume_button_event == 1;
        let stream_is_media =
            changed_streams.iter().find(|&&x| x.stream_type == AudioStreamType::Media).is_some();

        // Logging for debugging volume changes.
        if stream_is_media {
            fx_log_debug!("[earcons_agent] Volume up pressed while max: {}", volume_up_max_pressed);
            fx_log_debug!(
                "[earcons_agent] New media user volume: {:?}, Last media user volume: {:?}",
                new_media_user_volume,
                self.last_media_user_volume
            );
        }

        if ((self.last_media_user_volume != new_media_user_volume) || volume_up_max_pressed)
            && stream_is_media
        {
            if self.last_media_user_volume != None {
                // On restore, the last media user volume is set for the first time, and registers
                // as different from the last seen volume, because it is initially None. Don't play
                // the earcons sound on that set.
                self.play_media_volume_sound(new_media_user_volume);
            }
            self.last_media_user_volume = new_media_user_volume;
        }
    }

    /// Invoked when the background usage changes, determining whether a
    /// priority stream is playing.
    fn on_background_usage(&mut self, usage_request: UsageWatcherRequest) {
        let OnStateChanged { state, responder, .. } = usage_request;
        if responder.send().is_err() {
            fx_log_err!("could not send response for background usage");
        }
        self.priority_stream_playing = match state {
            Muted(_) | Ducked(_) => true,
            _ => false,
        };
    }

    /// Play the earcons sound given the changed volume streams.
    ///
    /// The parameters are packaged together. See [VolumeChangeParams].
    fn play_media_volume_sound(&self, volume: Option<f32>) {
        let common_earcons_params = self.common_earcons_params.clone();
        let priority_stream_playing = self.priority_stream_playing;

        fasync::spawn(async move {
            // Connect to the SoundPlayer if not already connected.
            connect_to_sound_player(
                common_earcons_params.service_context.clone(),
                common_earcons_params.sound_player_connection.clone(),
            )
            .await;

            let sound_player_connection_clone =
                common_earcons_params.sound_player_connection.clone();
            let sound_player_connection = sound_player_connection_clone.lock().await;
            let sound_player_added_files = common_earcons_params.sound_player_added_files;

            if let (Some(sound_player_proxy), Some(volume_level)) =
                (sound_player_connection.as_ref(), volume)
            {
                if priority_stream_playing {
                    fx_log_debug!("Detected a stream already playing, not playing earcons sound");
                    return;
                }

                if volume_level >= 1.0 {
                    play_sound(
                        &sound_player_proxy,
                        VOLUME_MAX_FILE_PATH,
                        VOLUME_MAX_SOUND_ID,
                        sound_player_added_files.clone(),
                    )
                    .await
                    .ok();
                } else if volume_level > 0.0 {
                    play_sound(
                        &sound_player_proxy,
                        VOLUME_CHANGED_FILE_PATH,
                        VOLUME_CHANGED_SOUND_ID,
                        sound_player_added_files.clone(),
                    )
                    .await
                    .ok();
                }
            }
        });
    }
}
