// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#[cfg(test)]
use {
    crate::agent::restore_agent::RestoreAgent,
    crate::audio::default_audio_info,
    crate::fidl_clone::FIDLClone,
    crate::registry::device_storage::testing::*,
    crate::registry::device_storage::{DeviceStorage, DeviceStorageFactory},
    crate::switchboard::base::{
        AudioInfo, AudioInputInfo, AudioSettingSource, AudioStream, AudioStreamType, SettingType,
    },
    crate::tests::fakes::audio_core_service::AudioCoreService,
    crate::tests::fakes::input_device_registry_service::InputDeviceRegistryService,
    crate::tests::fakes::service_registry::ServiceRegistry,
    crate::tests::fakes::sound_player_service::SoundPlayerService,
    crate::tests::fakes::usage_reporter_service::UsageReporterService,
    crate::EnvironmentBuilder,
    crate::Runtime,
    fidl_fuchsia_media::{
        AudioRenderUsage, Usage,
        UsageState::{Ducked, Muted, Unadjusted},
        UsageStateDucked, UsageStateMuted, UsageStateUnadjusted,
    },
    fidl_fuchsia_settings::*,
    fidl_fuchsia_ui_input::MediaButtonsEvent,
    fuchsia_component::server::NestedEnvironment,
    futures::lock::Mutex,
    std::sync::Arc,
};

const ENV_NAME: &str = "settings_service_audio_test_environment";

const CHANGED_VOLUME_LEVEL: f32 = 0.7;
const CHANGED_VOLUME_LEVEL_2: f32 = 0.8;
const MAX_VOLUME_LEVEL: f32 = 1.0;
const CHANGED_VOLUME_MUTED: bool = true;
const CHANGED_VOLUME_UNMUTED: bool = false;

const CHANGED_MEDIA_STREAM: AudioStream = AudioStream {
    stream_type: AudioStreamType::Media,
    source: AudioSettingSource::User,
    user_volume_level: CHANGED_VOLUME_LEVEL,
    user_volume_muted: CHANGED_VOLUME_MUTED,
};

const CHANGED_MEDIA_STREAM_SETTINGS: AudioStreamSettings = AudioStreamSettings {
    stream: Some(fidl_fuchsia_media::AudioRenderUsage::Media),
    source: Some(AudioStreamSettingSource::User),
    user_volume: Some(Volume {
        level: Some(CHANGED_VOLUME_LEVEL),
        muted: Some(CHANGED_VOLUME_MUTED),
    }),
};

const CHANGED_MEDIA_STREAM_SETTINGS_2: AudioStreamSettings = AudioStreamSettings {
    stream: Some(fidl_fuchsia_media::AudioRenderUsage::Media),
    source: Some(AudioStreamSettingSource::User),
    user_volume: Some(Volume {
        level: Some(CHANGED_VOLUME_LEVEL_2),
        muted: Some(CHANGED_VOLUME_MUTED),
    }),
};

const CHANGED_MEDIA_STREAM_SETTINGS_MAX: AudioStreamSettings = AudioStreamSettings {
    stream: Some(fidl_fuchsia_media::AudioRenderUsage::Media),
    source: Some(AudioStreamSettingSource::User),
    user_volume: Some(Volume {
        level: Some(MAX_VOLUME_LEVEL),
        muted: Some(CHANGED_VOLUME_UNMUTED),
    }),
};

const CHANGED_COMMUNICATION_STREAM_SETTINGS_MAX: AudioStreamSettings = AudioStreamSettings {
    stream: Some(fidl_fuchsia_media::AudioRenderUsage::Communication),
    source: Some(AudioStreamSettingSource::User),
    user_volume: Some(Volume {
        level: Some(MAX_VOLUME_LEVEL),
        muted: Some(CHANGED_VOLUME_UNMUTED),
    }),
};

const CHANGED_SYSTEM_AGENT_STREAM_SETTINGS_MAX: AudioStreamSettings = AudioStreamSettings {
    stream: Some(fidl_fuchsia_media::AudioRenderUsage::SystemAgent),
    source: Some(AudioStreamSettingSource::User),
    user_volume: Some(Volume {
        level: Some(MAX_VOLUME_LEVEL),
        muted: Some(CHANGED_VOLUME_UNMUTED),
    }),
};

// Used to store fake services for mocking dependencies and checking input/outputs.
// To add a new fake to these tests, add here, in create_services, and then use
// in your test.
struct FakeServices {
    audio_core: Arc<Mutex<AudioCoreService>>,
    input_device_registry: Arc<Mutex<InputDeviceRegistryService>>,
    sound_player: Arc<Mutex<SoundPlayerService>>,
    usage_reporter: Arc<Mutex<UsageReporterService>>,
}

fn get_default_stream(stream_type: AudioStreamType) -> AudioStream {
    *default_audio_info()
        .streams
        .iter()
        .find(|x| x.stream_type == stream_type)
        .expect("contains stream")
}

async fn set_volume(proxy: &AudioProxy, streams: Vec<AudioStreamSettings>) {
    let mut audio_settings = AudioSettings::empty();
    audio_settings.streams = Some(streams);
    proxy.set(audio_settings).await.expect("set completed").expect("set successful");
}

// Verifies that a stream equal to |stream| is inside of |settings|.
fn verify_audio_stream(settings: AudioSettings, stream: AudioStreamSettings) {
    settings
        .streams
        .expect("audio settings contain streams")
        .into_iter()
        .find(|x| *x == stream)
        .expect("contains stream");
}

// Gets the store from |factory| and populate it with default values.
async fn create_storage(
    factory: Arc<Mutex<InMemoryStorageFactory>>,
) -> Arc<Mutex<DeviceStorage<AudioInfo>>> {
    let store = factory.lock().await.get_store::<AudioInfo>();
    {
        let mut store_lock = store.lock().await;
        let audio_info = default_audio_info();
        store_lock.write(&audio_info, false).await.unwrap();
    }
    store
}

// Verify that |streams| contain |stream|.
fn verify_contains_stream(streams: &[AudioStream; 5], stream: &AudioStream) {
    streams.into_iter().find(|x| *x == stream).expect("contains changed media stream");
}

// Returns a registry and audio related services it is populated with
async fn create_services() -> (Arc<Mutex<ServiceRegistry>>, FakeServices) {
    let service_registry = ServiceRegistry::create();
    let audio_core_service_handle = Arc::new(Mutex::new(AudioCoreService::new()));
    service_registry.lock().await.register_service(audio_core_service_handle.clone());

    let input_device_registry_service_handle =
        Arc::new(Mutex::new(InputDeviceRegistryService::new()));
    service_registry.lock().await.register_service(input_device_registry_service_handle.clone());

    let sound_player_service_handle = Arc::new(Mutex::new(SoundPlayerService::new()));
    service_registry.lock().await.register_service(sound_player_service_handle.clone());

    let usage_reporter_service_handle = Arc::new(Mutex::new(UsageReporterService::new()));
    service_registry.lock().await.register_service(usage_reporter_service_handle.clone());

    (
        service_registry,
        FakeServices {
            audio_core: audio_core_service_handle,
            input_device_registry: input_device_registry_service_handle,
            sound_player: sound_player_service_handle,
            usage_reporter: usage_reporter_service_handle,
        },
    )
}

async fn create_environment(
    service_registry: Arc<Mutex<ServiceRegistry>>,
) -> (NestedEnvironment, Arc<Mutex<DeviceStorage<AudioInfo>>>) {
    let storage_factory = InMemoryStorageFactory::create_handle();
    let store = create_storage(storage_factory.clone()).await;

    let env = EnvironmentBuilder::new(Runtime::Nested(ENV_NAME), storage_factory)
        .service(ServiceRegistry::serve(service_registry))
        .settings(&[SettingType::Audio])
        .spawn_and_get_nested_environment()
        .await
        .unwrap();

    (env, store)
}

#[fuchsia_async::run_singlethreaded(test)]
async fn test_audio() {
    let (service_registry, fake_services) = create_services().await;
    let (env, store) = create_environment(service_registry).await;

    let audio_proxy = env.connect_to_service::<AudioMarker>().unwrap();

    let settings = audio_proxy.watch().await.expect("watch completed").expect("watch successful");
    verify_audio_stream(
        settings.clone(),
        AudioStreamSettings::from(get_default_stream(AudioStreamType::Media)),
    );

    set_volume(&audio_proxy, vec![CHANGED_MEDIA_STREAM_SETTINGS]).await;
    let settings = audio_proxy.watch().await.expect("watch completed").expect("watch successful");
    verify_audio_stream(settings.clone(), CHANGED_MEDIA_STREAM_SETTINGS);

    assert_eq!(
        (CHANGED_VOLUME_LEVEL, CHANGED_VOLUME_MUTED),
        fake_services.audio_core.lock().await.get_level_and_mute(AudioRenderUsage::Media).unwrap()
    );

    // Check to make sure value wrote out to store correctly.
    let stored_streams: [AudioStream; 5];
    {
        let mut store_lock = store.lock().await;
        stored_streams = store_lock.get().await.streams;
    }
    verify_contains_stream(&stored_streams, &CHANGED_MEDIA_STREAM);
}

// Tests that the volume level gets rounded to two decimal places.
#[fuchsia_async::run_singlethreaded(test)]
async fn test_volume_rounding() {
    let (service_registry, fake_services) = create_services().await;

    let (env, store) = create_environment(service_registry).await;

    let audio_proxy = env.connect_to_service::<AudioMarker>().unwrap();

    let settings = audio_proxy.watch().await.expect("watch completed").expect("watch successful");
    verify_audio_stream(
        settings.clone(),
        AudioStreamSettings::from(get_default_stream(AudioStreamType::Media)),
    );

    set_volume(
        &audio_proxy,
        vec![AudioStreamSettings {
            stream: Some(fidl_fuchsia_media::AudioRenderUsage::Media),
            source: Some(AudioStreamSettingSource::User),
            user_volume: Some(Volume { level: Some(0.7015), muted: Some(CHANGED_VOLUME_MUTED) }),
        }],
    )
    .await;

    let settings = audio_proxy.watch().await.expect("watch completed").expect("watch successful");
    verify_audio_stream(settings.clone(), CHANGED_MEDIA_STREAM_SETTINGS);

    assert_eq!(
        (CHANGED_VOLUME_LEVEL, CHANGED_VOLUME_MUTED),
        fake_services.audio_core.lock().await.get_level_and_mute(AudioRenderUsage::Media).unwrap()
    );

    // Check to make sure value wrote out to store correctly.
    let stored_streams: [AudioStream; 5];
    {
        let mut store_lock = store.lock().await;
        stored_streams = store_lock.get().await.streams;
    }
    verify_contains_stream(&stored_streams, &CHANGED_MEDIA_STREAM);
}

// Test to ensure mic input change events are received.
#[fuchsia_async::run_singlethreaded(test)]
async fn test_audio_input() {
    let (service_registry, fake_services) = create_services().await;

    let (env, _) = create_environment(service_registry).await;

    let audio_proxy = env.connect_to_service::<AudioMarker>().unwrap();

    let buttons_event = MediaButtonsEvent { volume: Some(1), mic_mute: Some(true) };
    fake_services.input_device_registry.lock().await.send_media_button_event(buttons_event.clone());

    let updated_settings =
        audio_proxy.watch().await.expect("watch completed").expect("watch successful");

    let input = updated_settings.input.expect("Should have input settings");
    let mic_mute = input.muted.expect("Should have mic mute value");
    assert!(mic_mute);
}

// Test to ensure that when the volume changes, the SoundPlayer receives requests to play the sounds
// with the correct ids.
#[fuchsia_async::run_singlethreaded(test)]
async fn test_sounds() {
    let (service_registry, fake_services) = create_services().await;
    let (env, _) = create_environment(service_registry).await;
    let audio_proxy = env.connect_to_service::<AudioMarker>().unwrap();

    // Test that the volume-changed sound gets played on the soundplayer.
    set_volume(&audio_proxy, vec![CHANGED_MEDIA_STREAM_SETTINGS_2]).await;
    let settings = audio_proxy.watch().await.expect("watch completed").expect("watch successful");
    verify_audio_stream(settings.clone(), CHANGED_MEDIA_STREAM_SETTINGS_2);
    assert!(fake_services.sound_player.lock().await.id_exists(1));
    assert_eq!(
        fake_services.sound_player.lock().await.get_usage_by_id(1),
        Some(AudioRenderUsage::Media)
    );

    // Test that the volume-max sound gets played on the soundplayer.
    set_volume(&audio_proxy, vec![CHANGED_MEDIA_STREAM_SETTINGS_MAX]).await;
    let settings = audio_proxy.watch().await.expect("watch completed").expect("watch successful");
    verify_audio_stream(settings.clone(), CHANGED_MEDIA_STREAM_SETTINGS_MAX);
    assert!(fake_services.sound_player.lock().await.id_exists(0));
    assert_eq!(
        fake_services.sound_player.lock().await.get_usage_by_id(0),
        Some(AudioRenderUsage::Media)
    );
}

// Test to ensure that when the volume is increased while already at max volume, the earcon for
// max volume plays.
#[fuchsia_async::run_singlethreaded(test)]
async fn test_max_volume_sound_on_press() {
    let (service_registry, fake_services) = create_services().await;
    let (env, _) = create_environment(service_registry).await;
    let audio_proxy = env.connect_to_service::<AudioMarker>().unwrap();

    // Set volume to max.
    set_volume(&audio_proxy, vec![CHANGED_MEDIA_STREAM_SETTINGS_MAX]).await;

    audio_proxy.watch().await.expect("watch completed").expect("watch successful");
    assert!(fake_services.sound_player.lock().await.id_exists(0));
    assert_eq!(fake_services.sound_player.lock().await.get_play_count(0), Some(1));

    // Try to increase volume. Only serves to set the "last volume button press" event
    // to 1 (volume up).
    let buttons_event = MediaButtonsEvent { volume: Some(1), mic_mute: Some(false) };
    fake_services.input_device_registry.lock().await.send_media_button_event(buttons_event.clone());

    // Sets volume max again.
    set_volume(&audio_proxy, vec![CHANGED_MEDIA_STREAM_SETTINGS_MAX]).await;

    audio_proxy.watch().await.expect("watch completed").expect("watch successful");

    // Check that the sound played more than once.
    assert!(fake_services.sound_player.lock().await.id_exists(0));
    assert!(fake_services.sound_player.lock().await.get_play_count(0).unwrap() > 1);
}

// Test to ensure that when the volume is changed on multiple channels, the sound only plays once.
#[fuchsia_async::run_singlethreaded(test)]
async fn test_earcons_on_multiple_channel_change() {
    let (service_registry, fake_services) = create_services().await;
    let (env, _) = create_environment(service_registry).await;
    let audio_proxy = env.connect_to_service::<AudioMarker>().unwrap();

    // Set volume to max on multiple channels.
    set_volume(&audio_proxy, vec![CHANGED_COMMUNICATION_STREAM_SETTINGS_MAX]).await;
    set_volume(&audio_proxy, vec![CHANGED_SYSTEM_AGENT_STREAM_SETTINGS_MAX]).await;
    set_volume(&audio_proxy, vec![CHANGED_MEDIA_STREAM_SETTINGS_MAX]).await;

    audio_proxy.watch().await.expect("watch completed").expect("watch successful");
    assert!(fake_services.sound_player.lock().await.id_exists(0));
    assert_eq!(fake_services.sound_player.lock().await.get_play_count(0), Some(1));
}

// Test to ensure that when another higher priority stream is playing,
// the earcons sounds don't play.
#[fuchsia_async::run_singlethreaded(test)]
async fn test_earcons_with_active_stream() {
    let (service_registry, fake_services) = create_services().await;
    let (env, _) = create_environment(service_registry).await;
    let audio_proxy = env.connect_to_service::<AudioMarker>().unwrap();

    set_volume(&audio_proxy, vec![CHANGED_MEDIA_STREAM_SETTINGS_2]).await;
    let settings = audio_proxy.watch().await.expect("watch completed").expect("watch successful");
    verify_audio_stream(settings.clone(), CHANGED_MEDIA_STREAM_SETTINGS_2);
    assert!(fake_services.sound_player.lock().await.id_exists(1));
    assert_eq!(
        fake_services.sound_player.lock().await.get_usage_by_id(1),
        Some(AudioRenderUsage::Media)
    );

    fake_services
        .usage_reporter
        .lock()
        .await
        .set_usage_state(
            Usage::RenderUsage(AudioRenderUsage::Background {}),
            Muted(UsageStateMuted {}),
        )
        .await;

    set_volume(&audio_proxy, vec![CHANGED_MEDIA_STREAM_SETTINGS_MAX]).await;
    let settings = audio_proxy.watch().await.expect("watch completed").expect("watch successful");
    verify_audio_stream(settings.clone(), CHANGED_MEDIA_STREAM_SETTINGS_MAX);

    // With the background stream muted, the sound should not have played.
    assert!(!fake_services.sound_player.lock().await.id_exists(0));
    assert_ne!(
        fake_services.sound_player.lock().await.get_usage_by_id(0),
        Some(AudioRenderUsage::Media)
    );

    fake_services
        .usage_reporter
        .lock()
        .await
        .set_usage_state(
            Usage::RenderUsage(AudioRenderUsage::Background {}),
            Ducked(UsageStateDucked {}),
        )
        .await;

    set_volume(&audio_proxy, vec![CHANGED_MEDIA_STREAM_SETTINGS_2]).await;
    audio_proxy.watch().await.expect("watch completed").expect("watch successful");

    set_volume(&audio_proxy, vec![CHANGED_MEDIA_STREAM_SETTINGS_MAX]).await;
    audio_proxy.watch().await.expect("watch completed").expect("watch successful");

    // With the background stream ducked, the sound should not have played.
    assert!(!fake_services.sound_player.lock().await.id_exists(0));
    assert_ne!(
        fake_services.sound_player.lock().await.get_usage_by_id(0),
        Some(AudioRenderUsage::Media)
    );

    fake_services
        .usage_reporter
        .lock()
        .await
        .set_usage_state(
            Usage::RenderUsage(AudioRenderUsage::Background {}),
            Unadjusted(UsageStateUnadjusted {}),
        )
        .await;

    set_volume(&audio_proxy, vec![CHANGED_MEDIA_STREAM_SETTINGS_2]).await;
    set_volume(&audio_proxy, vec![CHANGED_MEDIA_STREAM_SETTINGS_MAX]).await;

    audio_proxy.watch().await.expect("watch completed").expect("watch successful");

    // With the background stream unadjusted, the sound should have played.
    assert!(fake_services.sound_player.lock().await.id_exists(0));
    assert_eq!(
        fake_services.sound_player.lock().await.get_usage_by_id(0),
        Some(AudioRenderUsage::Media)
    );
}

/// Test that the audio settings are restored correctly.
#[fuchsia_async::run_singlethreaded(test)]
async fn test_volume_restore() {
    let (service_registry, fake_services) = create_services().await;
    let storage_factory = InMemoryStorageFactory::create_handle();
    let expected_info = (0.9, false);
    {
        let store = storage_factory.lock().await.get_store::<AudioInfo>();
        let mut stored_info = default_audio_info();
        for stream in stored_info.streams.iter_mut() {
            if stream.stream_type == AudioStreamType::Media {
                stream.user_volume_level = expected_info.0;
                stream.user_volume_muted = expected_info.1;
            }
        }
        assert!(store.lock().await.write(&stored_info, false).await.is_ok());
    }

    let env = EnvironmentBuilder::new(Runtime::Nested(ENV_NAME), storage_factory)
        .service(Box::new(ServiceRegistry::serve(service_registry)))
        .agents(&[Arc::new(Mutex::new(RestoreAgent::new()))])
        .settings(&[SettingType::Audio])
        .spawn()
        .unwrap();
    assert!(env.completion_rx.await.unwrap().is_ok());

    let stored_info =
        fake_services.audio_core.lock().await.get_level_and_mute(AudioRenderUsage::Media).unwrap();
    assert_eq!(stored_info, expected_info);
}

// Test to ensure mic input change events are received.
// TODO(fxb/41006): Add a request.
#[fuchsia_async::run_singlethreaded(test)]
async fn test_bringup_without_input_registry() {
    let service_registry = ServiceRegistry::create();
    let audio_core_service_handle = Arc::new(Mutex::new(AudioCoreService::new()));
    service_registry.lock().await.register_service(audio_core_service_handle.clone());

    let (env, _) = create_environment(service_registry).await;

    // At this point we should not crash.
    assert!(env.connect_to_service::<AudioMarker>().is_ok());
}

// Ensure that we won't crash if audio core fails.
#[fuchsia_async::run_singlethreaded(test)]
async fn test_bringup_without_audio_core() {
    let service_registry = ServiceRegistry::create();
    let input_registry_service_handle = Arc::new(Mutex::new(InputDeviceRegistryService::new()));
    service_registry.lock().await.register_service(input_registry_service_handle.clone());

    let (env, _) = create_environment(service_registry).await;

    // At this point we should not crash.
    let audio_proxy = env.connect_to_service::<AudioMarker>().unwrap();

    let settings = audio_proxy.watch().await.expect("watch completed").expect("watch successful");
    verify_audio_stream(
        settings.clone(),
        AudioStreamSettings::from(get_default_stream(AudioStreamType::Media)),
    );
}

#[fuchsia_async::run_singlethreaded(test)]
async fn test_audio_info_copy() {
    let audio_info = default_audio_info();
    let copy_audio_info = audio_info;
    assert_eq!(audio_info, copy_audio_info);
}

#[fuchsia_async::run_singlethreaded(test)]
async fn test_persisted_values_applied_at_start() {
    let (service_registry, fake_services) = create_services().await;
    let storage_factory = InMemoryStorageFactory::create_handle();
    let store = create_storage(storage_factory.clone()).await;

    let test_audio_info = AudioInfo {
        streams: [
            AudioStream {
                stream_type: AudioStreamType::Background,
                source: AudioSettingSource::User,
                user_volume_level: 0.5,
                user_volume_muted: true,
            },
            AudioStream {
                stream_type: AudioStreamType::Media,
                source: AudioSettingSource::User,
                user_volume_level: 0.6,
                user_volume_muted: true,
            },
            AudioStream {
                stream_type: AudioStreamType::Interruption,
                source: AudioSettingSource::System,
                user_volume_level: 0.3,
                user_volume_muted: false,
            },
            AudioStream {
                stream_type: AudioStreamType::SystemAgent,
                source: AudioSettingSource::User,
                user_volume_level: 0.7,
                user_volume_muted: true,
            },
            AudioStream {
                stream_type: AudioStreamType::Communication,
                source: AudioSettingSource::User,
                user_volume_level: 0.8,
                user_volume_muted: false,
            },
        ],
        input: AudioInputInfo { mic_mute: true },
    };

    // Write values in the store.
    {
        let mut store_lock = store.lock().await;
        store_lock.write(&test_audio_info, false).await.expect("write audio info in store");
    }

    let env = EnvironmentBuilder::new(Runtime::Nested(ENV_NAME), storage_factory)
        .service(ServiceRegistry::serve(service_registry))
        .agents(&[Arc::new(Mutex::new(RestoreAgent::new()))])
        .settings(&[SettingType::Audio])
        .spawn_and_get_nested_environment()
        .await
        .unwrap();

    let audio_proxy = env.connect_to_service::<AudioMarker>().unwrap();

    let settings = audio_proxy.watch().await.expect("watch completed").expect("watch successful");

    // Check that the stored values were returned from watch() and applied to the audio core
    // service.
    for stream in test_audio_info.streams.iter() {
        verify_audio_stream(settings.clone(), AudioStreamSettings::from(*stream));
        assert_eq!(
            (stream.user_volume_level, stream.user_volume_muted),
            fake_services
                .audio_core
                .lock()
                .await
                .get_level_and_mute(AudioRenderUsage::from(stream.stream_type))
                .unwrap()
        );
    }
}
