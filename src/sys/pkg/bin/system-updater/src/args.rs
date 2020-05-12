// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    argh::FromArgs,
    std::time::{Duration, SystemTime},
};

#[derive(Debug, Eq, FromArgs, PartialEq)]
/// Arguments for the system updater.
pub struct Args {
    /// what started this update: manual or automatic
    #[argh(option, default = "Initiator::Automatic")]
    pub initiator: Initiator,

    /// current OS version
    #[argh(option, default = "String::from(\"\")")]
    pub source: String,

    /// target OS version
    #[argh(option, default = "String::from(\"\")")]
    pub target: String,

    /// update package url
    #[argh(option, default = "String::from(\"fuchsia-pkg://fuchsia.com/update\")")]
    pub update: String,

    /// if true, reboot the system after successful OTA
    #[argh(option, default = "true")]
    pub reboot: bool,

    /// start time of update attempt, as unix nanosecond timestamp
    #[argh(option, from_str_fn(parse_wall_time))]
    pub start: Option<SystemTime>,
}

fn parse_wall_time(value: &str) -> Result<SystemTime, String> {
    let since_unix_epoch =
        Duration::from_nanos(value.parse().map_err(|e: std::num::ParseIntError| e.to_string())?);

    Ok(SystemTime::UNIX_EPOCH + since_unix_epoch)
}

#[derive(Debug, Clone, Copy, Eq, PartialEq)]
pub enum Initiator {
    Automatic,
    Manual,
}
impl std::str::FromStr for Initiator {
    type Err = anyhow::Error;
    fn from_str(s: &str) -> Result<Self, Self::Err> {
        match s {
            "automatic" => Ok(Initiator::Automatic),
            "manual" => Ok(Initiator::Manual),
            not_supported => Err(anyhow::anyhow!("initiator not supported: {:?}", not_supported)),
        }
    }
}

#[cfg(test)]
mod tests {
    use {super::*, matches::assert_matches};

    #[test]
    fn test_initiator() {
        assert_matches!(
            Args::from_args(&["system-updater"], &["--initiator", "manual"]),
            Ok(Args { initiator: Initiator::Manual, .. })
        );
    }

    #[test]
    fn test_unknown_initiator() {
        assert_matches!(
            Args::from_args(&["system-updater"], &["--initiator", "P Sherman"]),
            Err(_)
        );
    }

    #[test]
    fn test_start() {
        use chrono::prelude::*;

        const NANOS_IN_1970: u64 = 365 * 24 * 60 * 60 * 1_000_000_000;
        let nanos_in_1970 = NANOS_IN_1970.to_string();

        let args = Args::from_args(&["system-updater"], &["--start", &nanos_in_1970]).unwrap();
        assert_eq!(args.start, Some(Utc.ymd(1971, 1, 1).and_hms(0, 0, 0).into()));
    }

    #[test]
    fn test_source() {
        assert_matches!(
            Args::from_args(&["system-updater"], &["--source", "Wallaby Way"]),
            Ok(Args {
                source: source_version,
                ..
            }) if source_version=="Wallaby Way"
        );
    }

    #[test]
    fn test_target() {
        assert_matches!(
            Args::from_args(&["system-updater"], &["--target", "Sydney"]),
            Ok(Args {
                target: target_version,
                ..
            }) if target_version=="Sydney"
        );
    }

    #[test]
    fn test_update() {
        assert_matches!(
            Args::from_args(&["system-updater"], &["--update", "fuchsia-pkg://fuchsia.com/foo"]),
            Ok(Args {
                update: update_pkg_url,
                ..
            }) if update_pkg_url=="fuchsia-pkg://fuchsia.com/foo"
        );
    }

    #[test]
    fn test_reboot() {
        assert_matches!(
            Args::from_args(&["system-updater"], &["--reboot", "false"]),
            Ok(Args { reboot: false, .. })
        );
    }

    #[test]
    fn test_defaults() {
        assert_matches!(
            Args::from_args(&["system-updater"], &[]),
            Ok(Args {
                initiator: Initiator::Automatic,
                source: source_version,
                target: target_version,
                update: update_pkg_url,
                reboot: true,
                ..
            }) if source_version=="" &&
                  target_version=="" &&
                  update_pkg_url=="fuchsia-pkg://fuchsia.com/update"
        );
    }
}
