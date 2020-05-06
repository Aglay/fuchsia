#[cfg(test)]
use {
    crate::agent::earcons_agent::EarconsAgent,
    crate::agent::earcons_sound_ids::{
        BLUETOOTH_CONNECTED_SOUND_ID, BLUETOOTH_DISCONNECTED_SOUND_ID,
    },
    crate::agent::restore_agent::RestoreAgent,
    crate::registry::device_storage::testing::InMemoryStorageFactory,
    crate::tests::fakes::bluetooth_service::BluetoothService,
    crate::tests::fakes::fake_hanging_get_handler::HangingGetHandler,
    crate::tests::fakes::fake_hanging_get_types::ChangedPeers,
    crate::tests::fakes::service_registry::ServiceRegistry,
    crate::tests::fakes::sound_player_service::SoundPlayerService,
    crate::EnvironmentBuilder,
    anyhow::{format_err, Error},
    fidl_fuchsia_bluetooth::PeerId,
    fidl_fuchsia_bluetooth_sys::AccessWatchPeersResponder,
    fuchsia_component::server::NestedEnvironment,
    futures::channel::mpsc::UnboundedReceiver,
    futures::lock::Mutex,
    futures::StreamExt,
    std::sync::Arc,
};

const ENV_NAME: &str = "bluetooth_earcons_test_environment";

/// Used to store fake services for mocking dependencies and checking input/outputs.
/// To add a new fake to these tests, add here, in create_services, and then use
/// in your test.
#[allow(dead_code)]
struct FakeServices {
    sound_player: Arc<Mutex<SoundPlayerService>>,
    bluetooth: Arc<Mutex<BluetoothService>>,
    hanging_get_handler: Arc<Mutex<HangingGetHandler<ChangedPeers, AccessWatchPeersResponder>>>,
}

/// Builds the test environment.
async fn create_environment(service_registry: Arc<Mutex<ServiceRegistry>>) -> NestedEnvironment {
    let env = EnvironmentBuilder::new(InMemoryStorageFactory::create())
        .service(ServiceRegistry::serve(service_registry))
        .settings(&[])
        .agents(&[Arc::new(RestoreAgent::create), Arc::new(EarconsAgent::create)])
        .spawn_and_get_nested_environment(ENV_NAME)
        .await
        .unwrap();

    env
}

/// Creates and returns a registry and bluetooth related services it is populated with.
async fn create_services() -> (Arc<Mutex<ServiceRegistry>>, FakeServices) {
    let service_registry = ServiceRegistry::create();

    // Channel to send test updates on the bluetooth service.
    let (on_update_sender, on_update_receiver) =
        futures::channel::mpsc::unbounded::<ChangedPeers>();

    // Create a hanging get handler.
    let hanging_get_handler = HangingGetHandler::create(on_update_receiver).await;

    let sound_player_service_handle = Arc::new(Mutex::new(SoundPlayerService::new()));
    let bluetooth_service_handle =
        Arc::new(Mutex::new(BluetoothService::new(hanging_get_handler.clone(), on_update_sender)));
    service_registry.lock().await.register_service(sound_player_service_handle.clone());
    service_registry.lock().await.register_service(bluetooth_service_handle.clone());

    (
        service_registry,
        FakeServices {
            sound_player: sound_player_service_handle,
            bluetooth: bluetooth_service_handle,
            hanging_get_handler: hanging_get_handler,
        },
    )
}

/// Tests to ensure that when the bluetooth connections change, the SoundPlayer receives requests to play the sounds
/// with the correct ids.
#[fuchsia_async::run_singlethreaded(test)]
async fn test_sounds() {
    const PEER_ID_1: PeerId = PeerId { value: 1 };
    const PEER_ID_2: PeerId = PeerId { value: 2 };

    let (service_registry, fake_services) = create_services().await;
    let _env = create_environment(service_registry).await;

    // Create channel to receive notifications for when sounds are played. Used to know when to
    // check the sound player fake that the sound has been played.
    let (sound_played_sender, mut sound_played_receiver) =
        futures::channel::mpsc::unbounded::<Result<(), Error>>();
    fake_services.sound_player.lock().await.add_sound_played_listener(sound_played_sender).await;

    // Add first connection.
    fake_services.bluetooth.lock().await.connect(PEER_ID_1).await.ok();
    watch_for_next_sound_played(&mut sound_played_receiver).await.ok();
    assert!(fake_services.sound_player.lock().await.id_exists(BLUETOOTH_CONNECTED_SOUND_ID));
    assert_eq!(
        fake_services.sound_player.lock().await.get_play_count(BLUETOOTH_CONNECTED_SOUND_ID),
        Some(1)
    );

    // Add second connection.
    fake_services.bluetooth.lock().await.connect(PEER_ID_2).await.unwrap();
    watch_for_next_sound_played(&mut sound_played_receiver).await.ok();
    assert_eq!(
        fake_services.sound_player.lock().await.get_play_count(BLUETOOTH_CONNECTED_SOUND_ID),
        Some(2)
    );

    // Disconnect the first connection.
    fake_services.bluetooth.lock().await.disconnect(PEER_ID_1).await.unwrap();
    watch_for_next_sound_played(&mut sound_played_receiver).await.ok();
    assert!(fake_services.sound_player.lock().await.id_exists(BLUETOOTH_DISCONNECTED_SOUND_ID));
    assert_eq!(
        fake_services.sound_player.lock().await.get_play_count(BLUETOOTH_DISCONNECTED_SOUND_ID),
        Some(1)
    );

    // Disconnect the second connection.
    fake_services.bluetooth.lock().await.disconnect(PEER_ID_2).await.unwrap();
    watch_for_next_sound_played(&mut sound_played_receiver).await.ok();
    assert_eq!(
        fake_services.sound_player.lock().await.get_play_count(BLUETOOTH_DISCONNECTED_SOUND_ID),
        Some(2)
    );
}

/// Perform a watch on the sound player fake to wait until a sound has been played.
async fn watch_for_next_sound_played(
    sound_played_receiver: &mut UnboundedReceiver<Result<(), Error>>,
) -> Result<(), Error> {
    if let Some(response) = sound_played_receiver.next().await {
        response
    } else {
        Err(format_err!("No next event found in stream"))
    }
}
