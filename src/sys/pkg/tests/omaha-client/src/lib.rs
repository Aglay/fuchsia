// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![cfg(test)]
use {
    anyhow::anyhow,
    fidl_fuchsia_paver::PaverRequestStream,
    fidl_fuchsia_pkg::{PackageCacheRequestStream, PackageResolverRequestStream},
    fidl_fuchsia_update::{
        CheckNotStartedReason, CheckOptions, CheckingForUpdatesData, ErrorCheckingForUpdateData,
        Initiator, InstallationErrorData, InstallationProgress, InstallingData, ManagerMarker,
        ManagerProxy, MonitorMarker, MonitorRequest, MonitorRequestStream, NoUpdateAvailableData,
        State, UpdateInfo,
    },
    fidl_fuchsia_update_channelcontrol::{ChannelControlMarker, ChannelControlProxy},
    fidl_fuchsia_update_installer::InstallerMarker,
    fidl_fuchsia_update_installer_ext as installer, fuchsia_async as fasync,
    fuchsia_component::{
        client::{App, AppBuilder},
        server::{NestedEnvironment, ServiceFs},
    },
    fuchsia_inspect::{
        assert_inspect_tree, reader::DiagnosticsHierarchy, testing::TreeAssertion, tree_assertion,
    },
    fuchsia_pkg_testing::{get_inspect_hierarchy, make_packages_json},
    fuchsia_zircon as zx,
    futures::{
        channel::{mpsc, oneshot},
        prelude::*,
    },
    matches::assert_matches,
    mock_installer::MockUpdateInstallerService,
    mock_omaha_server::{OmahaResponse, OmahaServer},
    mock_paver::MockPaverServiceBuilder,
    mock_reboot::MockRebootService,
    mock_resolver::MockResolverService,
    parking_lot::Mutex,
    serde_json::json,
    std::{
        fs::{self, create_dir, File},
        path::PathBuf,
        sync::Arc,
    },
    tempfile::TempDir,
};

const OMAHA_CLIENT_CMX: &str =
    "fuchsia-pkg://fuchsia.com/omaha-client-integration-tests#meta/omaha-client-service-for-integration-test.cmx";
const SYSTEM_UPDATER_CMX: &str =
    "fuchsia-pkg://fuchsia.com/omaha-client-integration-tests#meta/system-updater-isolated.cmx";

struct Mounts {
    _test_dir: TempDir,
    config_data: PathBuf,
    build_info: PathBuf,
}

impl Mounts {
    fn new() -> Self {
        let test_dir = TempDir::new().expect("create test tempdir");
        let config_data = test_dir.path().join("config_data");
        create_dir(&config_data).expect("create config_data dir");
        let build_info = test_dir.path().join("build_info");
        create_dir(&build_info).expect("create build_info dir");

        Self { _test_dir: test_dir, config_data, build_info }
    }

    fn write_url(&self, url: impl AsRef<[u8]>) {
        let url_path = self.config_data.join("omaha_url");
        fs::write(url_path, url).expect("write omaha_url");
    }

    fn write_appid(&self, appid: impl AsRef<[u8]>) {
        let appid_path = self.config_data.join("omaha_app_id");
        fs::write(appid_path, appid).expect("write omaha_app_id");
    }

    fn write_policy_config(&self, config: impl AsRef<[u8]>) {
        let config_path = self.config_data.join("policy_config.json");
        fs::write(config_path, config).expect("write policy_config.json");
    }

    fn write_version(&self, version: impl AsRef<[u8]>) {
        let version_path = self.build_info.join("version");
        fs::write(version_path, version).expect("write version");
    }
}
struct Proxies {
    _cache: Arc<MockCache>,
    resolver: Arc<MockResolverService>,
    update_manager: ManagerProxy,
    channel_control: ChannelControlProxy,
}

struct TestEnvBuilder {
    response: OmahaResponse,
    version: String,
    installer: Option<MockUpdateInstallerService>,
}

impl TestEnvBuilder {
    fn new() -> Self {
        Self { response: OmahaResponse::NoUpdate, version: "0.1.2.3".to_string(), installer: None }
    }

    fn response(self, response: OmahaResponse) -> Self {
        Self { response, ..self }
    }

    fn version(self, version: impl Into<String>) -> Self {
        Self { version: version.into(), ..self }
    }

    fn installer(self, installer: MockUpdateInstallerService) -> Self {
        Self { installer: Some(installer), ..self }
    }

    fn build(self) -> TestEnv {
        let mounts = Mounts::new();

        let mut fs = ServiceFs::new();
        fs.add_proxy_service::<fidl_fuchsia_logger::LogSinkMarker, _>()
            .add_proxy_service::<fidl_fuchsia_posix_socket::ProviderMarker, _>();

        let server = OmahaServer::new(self.response);
        let url = server.start().expect("start server");
        mounts.write_url(url);
        mounts.write_appid("integration-test-appid");
        mounts.write_version(self.version);
        mounts.write_policy_config(
            json!({
                "startup_delay_seconds": 9999,
            })
            .to_string(),
        );

        let paver = Arc::new(MockPaverServiceBuilder::new().build());
        fs.add_fidl_service(move |stream: PaverRequestStream| {
            fasync::Task::spawn(
                Arc::clone(&paver)
                    .run_paver_service(stream)
                    .unwrap_or_else(|e| panic!("error running paver service: {:#}", anyhow!(e))),
            )
            .detach();
        });

        let resolver = Arc::new(MockResolverService::new(None));
        let resolver_clone = resolver.clone();
        fs.add_fidl_service(move |stream: PackageResolverRequestStream| {
            let resolver_clone = resolver_clone.clone();
            fasync::Task::spawn(
                Arc::clone(&resolver_clone)
                    .run_resolver_service(stream)
                    .unwrap_or_else(|e| panic!("error running resolver service {:#}", anyhow!(e))),
            )
            .detach()
        });

        let cache = Arc::new(MockCache::new());
        let cache_clone = cache.clone();
        fs.add_fidl_service(move |stream: PackageCacheRequestStream| {
            fasync::Task::spawn(Arc::clone(&cache_clone).run_cache_service(stream)).detach()
        });

        let (send, reboot_called) = oneshot::channel();
        let send = Mutex::new(Some(send));
        let reboot_service = Arc::new(MockRebootService::new(Box::new(move || {
            send.lock().take().unwrap().send(()).unwrap();
            Ok(())
        })));
        fs.add_fidl_service(move |stream| {
            fasync::Task::spawn(
                Arc::clone(&reboot_service)
                    .run_reboot_service(stream)
                    .unwrap_or_else(|e| panic!("error running reboot service: {:#}", anyhow!(e))),
            )
            .detach()
        });

        let nested_environment_label = Self::make_nested_environment_label();

        let (system_updater, env) = match self.installer {
            Some(installer) => {
                let installer = Arc::new(installer);
                let installer_clone = Arc::clone(&installer);
                fs.add_fidl_service(move |stream| {
                    fasync::Task::spawn(Arc::clone(&installer_clone).run_service(stream)).detach()
                });
                let env = fs
                    .create_nested_environment(&nested_environment_label)
                    .expect("nested environment to create successfully");
                (SystemUpdater::Mock(installer), env)
            }
            None => {
                let mut system_updater = AppBuilder::new(SYSTEM_UPDATER_CMX)
                    .add_dir_to_namespace(
                        "/config/build-info".into(),
                        File::open(&mounts.build_info).expect("open build_info"),
                    )
                    .unwrap();
                fs.add_proxy_service_to::<InstallerMarker, _>(
                    system_updater.directory_request().unwrap().clone(),
                );

                let env = fs
                    .create_nested_environment(&nested_environment_label)
                    .expect("nested environment to create successfully");
                (
                    SystemUpdater::Real(
                        system_updater.spawn(env.launcher()).expect("system_updater to launch"),
                    ),
                    env,
                )
            }
        };
        fasync::Task::spawn(fs.collect()).detach();

        let omaha_client = AppBuilder::new(OMAHA_CLIENT_CMX)
            .add_dir_to_namespace(
                "/config/data".into(),
                File::open(&mounts.config_data).expect("open config_data"),
            )
            .unwrap()
            .add_dir_to_namespace(
                "/config/build-info".into(),
                File::open(&mounts.build_info).expect("open build_info"),
            )
            .unwrap()
            .spawn(env.launcher())
            .expect("omaha_client to launch");

        TestEnv {
            _env: env,
            _mounts: mounts,
            proxies: Proxies {
                _cache: cache,
                resolver,
                update_manager: omaha_client
                    .connect_to_service::<ManagerMarker>()
                    .expect("connect to update manager"),
                channel_control: omaha_client
                    .connect_to_service::<ChannelControlMarker>()
                    .expect("connect to channel control"),
            },
            _omaha_client: omaha_client,
            _system_updater: system_updater,
            nested_environment_label,
            reboot_called,
        }
    }

    fn make_nested_environment_label() -> String {
        let mut salt = [0; 4];
        zx::cprng_draw(&mut salt[..]).expect("zx_cprng_draw does not fail");
        // omaha_client_integration_test_env_xxxxxxxx is too long and gets truncated.
        format!("omaha_client_test_env_{}", hex::encode(&salt))
    }
}

enum SystemUpdater {
    Real(App),
    Mock(Arc<MockUpdateInstallerService>),
}

struct TestEnv {
    _env: NestedEnvironment,
    _mounts: Mounts,
    proxies: Proxies,
    _omaha_client: App,
    _system_updater: SystemUpdater,
    nested_environment_label: String,
    reboot_called: oneshot::Receiver<()>,
}

impl TestEnv {
    async fn check_now(&self) -> MonitorRequestStream {
        let options = CheckOptions {
            initiator: Some(Initiator::User),
            allow_attaching_to_existing_update_check: Some(false),
            ..CheckOptions::EMPTY
        };
        let (client_end, stream) =
            fidl::endpoints::create_request_stream::<MonitorMarker>().unwrap();
        self.proxies
            .update_manager
            .check_now(options, Some(client_end))
            .await
            .expect("make check_now call")
            .expect("check started");
        stream
    }

    async fn perform_pending_reboot(&self) -> bool {
        self.proxies
            .update_manager
            .perform_pending_reboot()
            .await
            .expect("make perform_pending_reboot call")
    }

    async fn inspect_hierarchy(&self) -> DiagnosticsHierarchy {
        get_inspect_hierarchy(
            &self.nested_environment_label,
            "omaha-client-service-for-integration-test.cmx",
        )
        .await
    }

    async fn assert_platform_metrics(&self, children: TreeAssertion) {
        assert_inspect_tree!(
            self.inspect_hierarchy().await,
            "root": contains {
                "platform_metrics": {
                    "events": contains {
                        "capacity": 50u64,
                        children,
                    }
                }
            }
        );
    }
}

struct MockCache;

impl MockCache {
    fn new() -> Self {
        Self {}
    }
    async fn run_cache_service(self: Arc<Self>, mut stream: PackageCacheRequestStream) {
        while let Some(event) = stream.try_next().await.unwrap() {
            match event {
                fidl_fuchsia_pkg::PackageCacheRequest::Sync { responder } => {
                    responder.send(&mut Ok(())).unwrap();
                }
                other => panic!("unsupported PackageCache request: {:?}", other),
            }
        }
    }
}

async fn expect_states(stream: &mut MonitorRequestStream, expected_states: &[State]) {
    for expected_state in expected_states {
        let MonitorRequest::OnState { state, responder } =
            stream.try_next().await.unwrap().unwrap();
        assert_eq!(&state, expected_state);
        responder.send().unwrap();
    }
}

fn update_info() -> Option<UpdateInfo> {
    // TODO(fxbug.dev/47469): version_available should be `Some("0.1.2.3".to_string())` once omaha-client
    // returns version_available.
    Some(UpdateInfo { version_available: None, download_size: None, ..UpdateInfo::EMPTY })
}

fn progress(fraction_completed: Option<f32>) -> Option<InstallationProgress> {
    Some(InstallationProgress { fraction_completed, ..InstallationProgress::EMPTY })
}

#[fasync::run_singlethreaded(test)]
async fn test_omaha_client_update() {
    let mut env = TestEnvBuilder::new().response(OmahaResponse::Update).build();

    env.proxies
        .resolver
        .url("fuchsia-pkg://integration.test.fuchsia.com/update?hash=deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef")
        .resolve(
        &env.proxies
            .resolver
            .package("update", "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef")
            .add_file(
                "packages.json",
                make_packages_json(["fuchsia-pkg://fuchsia.com/system_image/0?hash=beefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdead"]),
            )
            .add_file("zbi", "fake zbi"),
    );
    env.proxies
        .resolver.url("fuchsia-pkg://fuchsia.com/system_image/0?hash=beefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdead")
        .resolve(
        &env.proxies
            .resolver
            .package("system_image", "beefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeada")
    );

    let mut stream = env.check_now().await;
    expect_states(
        &mut stream,
        &[
            State::CheckingForUpdates(CheckingForUpdatesData::EMPTY),
            State::InstallingUpdate(InstallingData {
                update: update_info(),
                installation_progress: progress(None),
                ..InstallingData::EMPTY
            }),
        ],
    )
    .await;
    let mut last_progress: Option<InstallationProgress> = None;
    let mut waiting_for_reboot = false;
    while let Some(request) = stream.try_next().await.unwrap() {
        let MonitorRequest::OnState { state, responder } = request;
        match state {
            State::InstallingUpdate(InstallingData { update, installation_progress, .. }) => {
                assert_eq!(update, update_info());
                assert!(!waiting_for_reboot);
                if let Some(last_progress) = last_progress {
                    let last = last_progress.fraction_completed.unwrap();
                    let current =
                        installation_progress.as_ref().unwrap().fraction_completed.unwrap();
                    assert!(
                        last <= current,
                        "progress is not increasing, last: {}, current: {}",
                        last,
                        current,
                    );
                }
                last_progress = installation_progress;
            }
            State::WaitingForReboot(InstallingData { update, installation_progress, .. }) => {
                assert_eq!(update, update_info());
                assert_eq!(installation_progress, progress(Some(1.)));
                assert!(!waiting_for_reboot);
                waiting_for_reboot = true;
                assert_matches!(env.reboot_called.try_recv(), Ok(None));
            }
            state => {
                panic!("Unexpected state: {:?}", state);
            }
        }
        responder.send().unwrap();
    }
    assert_matches!(last_progress, Some(_));
    assert!(waiting_for_reboot);

    env.assert_platform_metrics(tree_assertion!(
        "children": {
            "0": contains {
                "event": "CheckingForUpdates",
            },
            "1": contains {
                "event": "InstallingUpdate",
                "target-version": "0.1.2.3",
            },
            "2": contains {
                "event": "WaitingForReboot",
                "target-version": "0.1.2.3",
            }
        }
    ))
    .await;

    // This will hang if reboot was not triggered.
    env.reboot_called.await.unwrap();
}

#[fasync::run_singlethreaded(test)]
async fn test_omaha_client_update_progress_with_mock_installer() {
    let (mut sender, receiver) = mpsc::channel(0);
    let installer = MockUpdateInstallerService::builder().states_receiver(receiver).build();
    let env = TestEnvBuilder::new().response(OmahaResponse::Update).installer(installer).build();

    let mut stream = env.check_now().await;

    expect_states(
        &mut stream,
        &[
            State::CheckingForUpdates(CheckingForUpdatesData::EMPTY),
            State::InstallingUpdate(InstallingData {
                update: update_info(),
                installation_progress: progress(None),
                ..InstallingData::EMPTY
            }),
        ],
    )
    .await;

    // Send installer state and expect manager step in lockstep to make sure that event queue
    // won't merge any progress.
    sender.send(installer::State::Prepare).await.unwrap();
    expect_states(
        &mut stream,
        &[State::InstallingUpdate(InstallingData {
            update: update_info(),
            installation_progress: progress(Some(0.0)),
            ..InstallingData::EMPTY
        })],
    )
    .await;

    let installer_update_info = installer::UpdateInfo::builder().download_size(1000).build();
    sender
        .send(installer::State::Fetch(
            installer::UpdateInfoAndProgress::new(
                installer_update_info,
                installer::Progress::none(),
            )
            .unwrap(),
        ))
        .await
        .unwrap();
    expect_states(
        &mut stream,
        &[State::InstallingUpdate(InstallingData {
            update: update_info(),
            installation_progress: progress(Some(0.0)),
            ..InstallingData::EMPTY
        })],
    )
    .await;

    sender
        .send(installer::State::Stage(
            installer::UpdateInfoAndProgress::new(
                installer_update_info,
                installer::Progress::builder()
                    .fraction_completed(0.5)
                    .bytes_downloaded(500)
                    .build(),
            )
            .unwrap(),
        ))
        .await
        .unwrap();
    expect_states(
        &mut stream,
        &[State::InstallingUpdate(InstallingData {
            update: update_info(),
            installation_progress: progress(Some(0.5)),
            ..InstallingData::EMPTY
        })],
    )
    .await;

    sender
        .send(installer::State::WaitToReboot(installer::UpdateInfoAndProgress::done(
            installer_update_info,
        )))
        .await
        .unwrap();
    expect_states(
        &mut stream,
        &[
            State::InstallingUpdate(InstallingData {
                update: update_info(),
                installation_progress: progress(Some(1.0)),
                ..InstallingData::EMPTY
            }),
            State::WaitingForReboot(InstallingData {
                update: update_info(),
                installation_progress: progress(Some(1.0)),
                ..InstallingData::EMPTY
            }),
        ],
    )
    .await;
}

#[fasync::run_singlethreaded(test)]
async fn test_omaha_client_update_error() {
    let env = TestEnvBuilder::new().response(OmahaResponse::Update).build();

    let mut stream = env.check_now().await;
    expect_states(
        &mut stream,
        &[
            State::CheckingForUpdates(CheckingForUpdatesData::EMPTY),
            State::InstallingUpdate(InstallingData {
                update: update_info(),
                installation_progress: progress(None),
                ..InstallingData::EMPTY
            }),
        ],
    )
    .await;
    let mut last_progress: Option<InstallationProgress> = None;
    let mut installation_error = false;
    while let Some(request) = stream.try_next().await.unwrap() {
        let MonitorRequest::OnState { state, responder } = request;
        match state {
            State::InstallingUpdate(InstallingData { update, installation_progress, .. }) => {
                assert_eq!(update, update_info());
                assert!(!installation_error);
                if let Some(last_progress) = last_progress {
                    let last = last_progress.fraction_completed.unwrap();
                    let current =
                        installation_progress.as_ref().unwrap().fraction_completed.unwrap();
                    assert!(
                        last <= current,
                        "progress is not increasing, last: {}, current: {}",
                        last,
                        current,
                    );
                }
                last_progress = installation_progress;
            }

            State::InstallationError(InstallationErrorData {
                update,
                installation_progress: _,
                ..
            }) => {
                assert_eq!(update, update_info());
                assert!(!installation_error);
                installation_error = true;
            }
            state => {
                panic!("Unexpected state: {:?}", state);
            }
        }
        responder.send().unwrap();
    }
    assert!(installation_error);

    env.assert_platform_metrics(tree_assertion!(
        "children": {
            "0": contains {
                "event": "CheckingForUpdates",
            },
            "1": contains {
                "event": "InstallingUpdate",
                "target-version": "0.1.2.3",
            },
            "2": contains {
                "event": "InstallationError",
                "target-version": "0.1.2.3",
            }
        }
    ))
    .await;
}

#[fasync::run_singlethreaded(test)]
async fn test_omaha_client_no_update() {
    let env = TestEnvBuilder::new().build();

    let mut stream = env.check_now().await;
    expect_states(
        &mut stream,
        &[
            State::CheckingForUpdates(CheckingForUpdatesData::EMPTY),
            State::NoUpdateAvailable(NoUpdateAvailableData::EMPTY),
        ],
    )
    .await;
    assert_matches!(stream.next().await, None);

    env.assert_platform_metrics(tree_assertion!(
        "children": {
            "0": contains {
                "event": "CheckingForUpdates",
            },
            "1": contains {
                "event": "NoUpdateAvailable",
            },
        }
    ))
    .await;
}

#[fasync::run_singlethreaded(test)]
async fn test_omaha_client_invalid_response() {
    let env = TestEnvBuilder::new().response(OmahaResponse::InvalidResponse).build();

    let mut stream = env.check_now().await;
    expect_states(
        &mut stream,
        &[
            State::CheckingForUpdates(CheckingForUpdatesData::EMPTY),
            State::ErrorCheckingForUpdate(ErrorCheckingForUpdateData::EMPTY),
        ],
    )
    .await;
    assert_matches!(stream.next().await, None);

    env.assert_platform_metrics(tree_assertion!(
        "children": {
            "0": contains {
                "event": "CheckingForUpdates",
            },
            "1": contains {
                "event": "ErrorCheckingForUpdate",
            }
        }
    ))
    .await;
}

#[fasync::run_singlethreaded(test)]
async fn test_omaha_client_invalid_url() {
    let env = TestEnvBuilder::new().response(OmahaResponse::InvalidURL).build();

    let mut stream = env.check_now().await;
    expect_states(
        &mut stream,
        &[
            State::CheckingForUpdates(CheckingForUpdatesData::EMPTY),
            State::InstallingUpdate(InstallingData {
                update: update_info(),
                installation_progress: progress(None),
                ..InstallingData::EMPTY
            }),
            State::InstallationError(InstallationErrorData {
                update: update_info(),
                installation_progress: progress(None),
                ..InstallationErrorData::EMPTY
            }),
        ],
    )
    .await;
    assert_matches!(stream.next().await, None);

    env.assert_platform_metrics(tree_assertion!(
        "children": {
            "0": contains {
                "event": "CheckingForUpdates",
            },
            "1": contains {
                "event": "InstallingUpdate",
                "target-version": "0.1.2.3",
            },
            "2": contains {
                "event": "InstallationError",
                "target-version": "0.1.2.3",
            }
        }
    ))
    .await;
}

#[fasync::run_singlethreaded(test)]
async fn test_omaha_client_invalid_app_set() {
    let env = TestEnvBuilder::new().version("invalid-version").build();

    let options = CheckOptions {
        initiator: Some(Initiator::User),
        allow_attaching_to_existing_update_check: None,
        ..CheckOptions::EMPTY
    };
    assert_matches!(
        env.proxies.update_manager.check_now(options, None).await.expect("check_now"),
        Err(CheckNotStartedReason::Internal)
    );
}

#[fasync::run_singlethreaded(test)]
async fn test_omaha_client_policy_config_inspect() {
    let env = TestEnvBuilder::new().build();

    // Wait for omaha client to start.
    let _ = env.proxies.channel_control.get_current().await;

    assert_inspect_tree!(
        env.inspect_hierarchy().await,
        "root": contains {
            "policy_config": {
                "periodic_interval": 60 * 60u64,
                "startup_delay": 9999u64,
                "retry_delay": 5 * 60u64,
                "allow_reboot_when_idle": true,
            }
        }
    );
}

#[fasync::run_singlethreaded(test)]
async fn test_omaha_client_perform_pending_reboot_after_out_of_space() {
    let env = TestEnvBuilder::new().response(OmahaResponse::Update).build();

    // We should be able to get the update package just fine
    env.proxies
        .resolver
        .url("fuchsia-pkg://integration.test.fuchsia.com/update?hash=deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef")
        .resolve(
        &env.proxies
            .resolver
            .package("update", "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef")
            .add_file(
                "packages.json",
                make_packages_json(["fuchsia-pkg://fuchsia.com/system_image/0?hash=beefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdead"]),
            )
            .add_file("zbi", "fake zbi"),
    );

    // ...but the system image package should fail with NO_SPACE
    env.proxies
        .resolver.url("fuchsia-pkg://fuchsia.com/system_image/0?hash=beefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdead")
        .fail(zx::Status::NO_SPACE);

    let mut stream = env.check_now().await;

    // Consume the initial states
    expect_states(
        &mut stream,
        &[
            State::CheckingForUpdates(CheckingForUpdatesData::EMPTY),
            State::InstallingUpdate(InstallingData {
                update: update_info(),
                installation_progress: progress(None),
                ..InstallingData::EMPTY
            }),
        ],
    )
    .await;

    // Monitor the installation until we get an installation error.
    let mut installation_error = false;
    while let Some(request) = stream.try_next().await.unwrap() {
        let MonitorRequest::OnState { state, responder } = request;
        match state {
            State::InstallingUpdate(InstallingData { update, .. }) => {
                assert_eq!(update, update_info());
            }
            State::InstallationError(InstallationErrorData { update, .. }) => {
                assert_eq!(update, update_info());
                assert!(!installation_error);
                installation_error = true;
            }
            state => {
                panic!("Unexpected state: {:?}", state);
            }
        }
        responder.send().unwrap();
    }
    assert!(installation_error);

    // Simulate an incoming call to PerformPendingReboot. It returns true if we're rebooting.
    assert!(env.perform_pending_reboot().await);

    // This will hang if reboot was not triggered.
    env.reboot_called.await.unwrap();
}
