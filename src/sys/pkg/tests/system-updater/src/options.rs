// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {super::*, pretty_assertions::assert_eq};

#[fasync::run_singlethreaded(test)]
async fn uses_custom_update_package() {
    let env = TestEnv::new();

    env.resolver
        .register_custom_package("another-update/4", "update", "upd4t3r", "fuchsia.com")
        .add_file("packages.json", make_packages_json([]))
        .add_file("zbi", "fake zbi");

    env.run_system_updater(SystemUpdaterArgs {
        initiator: Some(Initiator::User),
        target: Some("m3rk13"),
        update: Some("fuchsia-pkg://fuchsia.com/another-update/4"),
        oneshot: Some(true),
        ..Default::default()
    })
    .await
    .expect("run system updater");

    assert_eq!(
        env.take_interactions(),
        vec![
            Gc,
            PackageResolve("fuchsia-pkg://fuchsia.com/another-update/4".to_string()),
            Gc,
            BlobfsSync,
            Paver(PaverEvent::QueryActiveConfiguration),
            Paver(PaverEvent::WriteAsset {
                configuration: paver::Configuration::B,
                asset: paver::Asset::Kernel,
                payload: b"fake zbi".to_vec(),
            }),
            Paver(PaverEvent::SetConfigurationActive { configuration: paver::Configuration::B }),
            Paver(PaverEvent::DataSinkFlush),
            Paver(PaverEvent::BootManagerFlush),
            Reboot,
        ]
    );
}

#[fasync::run_singlethreaded(test)]
async fn rejects_invalid_update_package_url() {
    let env = TestEnv::new();

    let bogus_url = "not-fuchsia-pkg://fuchsia.com/not-a-update";

    env.resolver.mock_resolve_failure(bogus_url, Status::INVALID_ARGS);

    let result = env
        .run_system_updater(SystemUpdaterArgs {
            initiator: Some(Initiator::User),
            target: Some("m3rk13"),
            update: Some(bogus_url),
            oneshot: Some(true),
            ..Default::default()
        })
        .await;
    assert!(result.is_err(), "system updater succeeded when it should fail");

    assert_eq!(env.take_interactions(), vec![]);
}

#[fasync::run_singlethreaded(test)]
async fn rejects_unknown_flags() {
    let env = TestEnv::new();

    env.resolver
        .register_package("update", "upd4t3")
        .add_file(
            "packages",
            "system_image/0=42ade6f4fd51636f70c68811228b4271ed52c4eb9a647305123b4f4d0741f296\n",
        )
        .add_file("zbi", "fake zbi");

    let result = env
        .run_system_updater_args(
            RawSystemUpdaterArgs(&["--initiator", "manual", "--target", "m3rk13", "--foo", "bar"]),
            Default::default(),
        )
        .await;
    assert!(result.is_err(), "system updater succeeded when it should fail");
    assert_eq!(env.take_interactions(), vec![]);
}

#[fasync::run_singlethreaded(test)]
async fn rejects_extra_args() {
    let env = TestEnv::new();

    env.resolver
        .register_package("update", "upd4t3")
        .add_file(
            "packages",
            "system_image/0=42ade6f4fd51636f70c68811228b4271ed52c4eb9a647305123b4f4d0741f296\n",
        )
        .add_file("zbi", "fake zbi");

    let result = env
        .run_system_updater_args(
            RawSystemUpdaterArgs(&["--initiator", "manual", "--target", "m3rk13", "foo"]),
            Default::default(),
        )
        .await;
    assert!(result.is_err(), "system updater succeeded when it should fail");

    assert_eq!(env.take_interactions(), vec![]);
}

#[fasync::run_singlethreaded(test)]
async fn does_not_reboot_if_requested_not_to_reboot() {
    let env = TestEnv::new();

    env.resolver
        .register_package("update", "upd4t3")
        .add_file("packages.json", make_packages_json([]))
        .add_file("zbi", "fake zbi");

    env.run_system_updater(SystemUpdaterArgs {
        initiator: Some(Initiator::User),
        target: Some("m3rk13"),
        reboot: Some(false),
        oneshot: Some(true),
        ..Default::default()
    })
    .await
    .expect("run system updater");

    let loggers = env.logger_factory.loggers.lock().clone();
    assert_eq!(loggers.len(), 1);
    let logger = loggers.into_iter().next().unwrap();
    assert_eq!(
        OtaMetrics::from_events(logger.cobalt_events.lock().clone()),
        OtaMetrics {
            initiator: metrics::OtaResultAttemptsMetricDimensionInitiator::UserInitiatedCheck
                as u32,
            phase: metrics::OtaResultAttemptsMetricDimensionPhase::SuccessPendingReboot as u32,
            status_code: metrics::OtaResultAttemptsMetricDimensionStatusCode::Success as u32,
            target: "m3rk13".into(),
        }
    );

    assert_eq!(
        env.take_interactions(),
        vec![
            Gc,
            PackageResolve(UPDATE_PKG_URL.to_string()),
            Gc,
            BlobfsSync,
            Paver(PaverEvent::QueryActiveConfiguration),
            Paver(PaverEvent::WriteAsset {
                configuration: paver::Configuration::B,
                asset: paver::Asset::Kernel,
                payload: b"fake zbi".to_vec(),
            }),
            Paver(PaverEvent::SetConfigurationActive { configuration: paver::Configuration::B }),
            Paver(PaverEvent::DataSinkFlush),
            Paver(PaverEvent::BootManagerFlush),
        ]
    );
}

#[fasync::run_singlethreaded(test)]
async fn oneshot_false_not_implemented() {
    let env = TestEnv::new();

    env.resolver
        .register_package("update", "upd4t3")
        .add_file(
            "packages",
            "system_image/0=42ade6f4fd51636f70c68811228b4271ed52c4eb9a647305123b4f4d0741f296\n",
        )
        .add_file("zbi", "fake zbi");

    let result = env.run_system_updater(SystemUpdaterArgs::default()).await;
    assert!(result.is_err(), "system updater succeeded when it should fail");

    assert_eq!(env.take_interactions(), vec![]);

    let result = env
        .run_system_updater(SystemUpdaterArgs { oneshot: Some(false), ..Default::default() })
        .await;
    assert!(result.is_err(), "system updater succeeded when it should fail");

    assert_eq!(env.take_interactions(), vec![]);
}
