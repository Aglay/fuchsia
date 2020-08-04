// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Wrapper types for the State union.

use {
    fidl_fuchsia_update_installer as fidl,
    proptest::prelude::*,
    proptest_derive::Arbitrary,
    serde::{Deserialize, Serialize},
    std::convert::{TryFrom, TryInto},
    thiserror::Error,
};

/// The state of an update installation attempt.
#[derive(Arbitrary, Clone, Debug, Serialize, PartialEq)]
#[serde(tag = "id", rename_all = "snake_case")]
#[allow(missing_docs)]
pub enum State {
    Prepare,

    #[proptest(strategy = "arb_state_fetch()")]
    Fetch {
        info: UpdateInfo,
        progress: Progress,
    },

    #[proptest(strategy = "arb_state_stage()")]
    Stage {
        info: UpdateInfo,
        progress: Progress,
    },

    #[proptest(strategy = "arb_state_wait_to_reboot()")]
    WaitToReboot {
        info: UpdateInfo,
        progress: Progress,
    },

    #[proptest(strategy = "arb_state_reboot()")]
    Reboot {
        info: UpdateInfo,
        progress: Progress,
    },

    #[proptest(strategy = "arb_state_defer_reboot()")]
    DeferReboot {
        info: UpdateInfo,
        progress: Progress,
    },

    #[proptest(strategy = "arb_state_complete()")]
    Complete {
        info: UpdateInfo,
        progress: Progress,
    },

    FailPrepare,

    #[proptest(strategy = "arb_state_fail_fetch()")]
    FailFetch {
        info: UpdateInfo,
        progress: Progress,
    },

    #[proptest(strategy = "arb_state_fail_stage()")]
    FailStage {
        info: UpdateInfo,
        progress: Progress,
    },
}

#[derive(Clone, Copy, Debug, PartialEq)]
enum StateId {
    Prepare,
    Fetch,
    Stage,
    WaitToReboot,
    Reboot,
    DeferReboot,
    Complete,
    FailPrepare,
    FailFetch,
    FailStage,
}

/// Immutable metadata for an update attempt.
#[derive(Arbitrary, Clone, Copy, Debug, Serialize, Deserialize, PartialEq, PartialOrd)]
pub struct UpdateInfo {
    download_size: u64,
}

/// Builder of UpdateInfo
#[derive(Clone, Debug)]
pub struct UpdateInfoBuilder;

/// Builder of UpdateInfo, with a known download_size field.
#[derive(Clone, Debug)]
pub struct UpdateInfoBuilderWithDownloadSize {
    download_size: u64,
}

/// Mutable progress information for an update attempt.
#[derive(Arbitrary, Clone, Debug, Serialize, PartialEq, PartialOrd)]
pub struct Progress {
    /// Within the range of [0.0, 1.0]
    #[proptest(strategy = "0.0f32 ..= 1.0")]
    fraction_completed: f32,

    bytes_downloaded: u64,
}

/// Builder of Progress.
#[derive(Clone, Debug)]
pub struct ProgressBuilder;

/// Builder of Progress, with a known fraction_completed field.
#[derive(Clone, Debug)]
pub struct ProgressBuilderWithFraction {
    fraction_completed: f32,
}

/// Builder of Progress, with a known fraction_completed and bytes_downloaded field.
#[derive(Clone, Debug)]
pub struct ProgressBuilderWithFractionAndBytes {
    fraction_completed: f32,
    bytes_downloaded: u64,
}

impl State {
    fn id(&self) -> StateId {
        match self {
            State::Prepare => StateId::Prepare,
            State::Fetch { .. } => StateId::Fetch,
            State::Stage { .. } => StateId::Stage,
            State::WaitToReboot { .. } => StateId::WaitToReboot,
            State::Reboot { .. } => StateId::Reboot,
            State::DeferReboot { .. } => StateId::DeferReboot,
            State::Complete { .. } => StateId::Complete,
            State::FailPrepare => StateId::FailPrepare,
            State::FailFetch { .. } => StateId::FailFetch,
            State::FailStage { .. } => StateId::FailStage,
        }
    }

    /// Determines if this state is terminal and represents a successful attempt.
    pub fn is_success(&self) -> bool {
        match self.id() {
            StateId::Reboot | StateId::DeferReboot | StateId::Complete => true,
            _ => false,
        }
    }

    /// Determines if this state is terminal and represents a failure.
    pub fn is_failure(&self) -> bool {
        match self.id() {
            StateId::FailPrepare | StateId::FailFetch | StateId::FailStage => true,
            _ => false,
        }
    }

    /// Determines if this state is terminal (terminal states are final, no futher state
    /// transitions should occur).
    pub fn is_terminal(&self) -> bool {
        self.is_success() || self.is_failure()
    }
}

impl UpdateInfo {
    /// Starts building an instance of UpdateInfo.
    pub fn builder() -> UpdateInfoBuilder {
        UpdateInfoBuilder
    }

    /// Gets the download_size field.
    pub fn download_size(&self) -> u64 {
        self.download_size
    }
}

impl UpdateInfoBuilder {
    /// Sets the download_size field.
    pub fn download_size(self, download_size: u64) -> UpdateInfoBuilderWithDownloadSize {
        UpdateInfoBuilderWithDownloadSize { download_size }
    }
}

impl UpdateInfoBuilderWithDownloadSize {
    /// Builds the UpdateInfo instance.
    pub fn build(self) -> UpdateInfo {
        let Self { download_size } = self;
        UpdateInfo { download_size }
    }
}

impl Progress {
    /// Starts building an instance of Progress.
    pub fn builder() -> ProgressBuilder {
        ProgressBuilder
    }

    /// Produces a Progress at 0% complete and 0 bytes downloaded.
    pub fn none() -> Self {
        Self { fraction_completed: 0.0, bytes_downloaded: 0 }
    }

    /// Produces a Progress at 100% complete and all bytes downloaded, based on the download_size
    /// in `info`.
    pub fn done(info: &UpdateInfo) -> Self {
        Self { fraction_completed: 1.0, bytes_downloaded: info.download_size }
    }

    /// Gets the fraction_completed field.
    pub fn fraction_completed(&self) -> f32 {
        self.fraction_completed
    }

    /// Gets the bytes_downloaded field.
    pub fn bytes_downloaded(&self) -> u64 {
        self.bytes_downloaded
    }
}

impl ProgressBuilder {
    /// Sets the fraction_completed field, claming the provided float to the range [0.0, 1.0] and
    /// converting NaN to 0.0.
    pub fn fraction_completed(self, fraction_completed: f32) -> ProgressBuilderWithFraction {
        ProgressBuilderWithFraction { fraction_completed: fraction_completed.max(0.0).min(1.0) }
    }
}

impl ProgressBuilderWithFraction {
    /// Sets the bytes_downloaded field.
    pub fn bytes_downloaded(self, bytes_downloaded: u64) -> ProgressBuilderWithFractionAndBytes {
        ProgressBuilderWithFractionAndBytes {
            fraction_completed: self.fraction_completed,
            bytes_downloaded,
        }
    }
}

impl ProgressBuilderWithFractionAndBytes {
    /// Builds the Progress instance.
    pub fn build(self) -> Progress {
        let Self { fraction_completed, bytes_downloaded } = self;
        Progress { fraction_completed, bytes_downloaded }
    }
}

impl<'de> Deserialize<'de> for State {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: serde::Deserializer<'de>,
    {
        use serde::de::Error;

        #[derive(Debug, Deserialize)]
        #[serde(tag = "id", rename_all = "snake_case")]
        enum DeState {
            Prepare,
            Fetch { info: UpdateInfo, progress: Progress },
            Stage { info: UpdateInfo, progress: Progress },
            WaitToReboot { info: UpdateInfo, progress: Progress },
            Reboot { info: UpdateInfo, progress: Progress },
            DeferReboot { info: UpdateInfo, progress: Progress },
            Complete { info: UpdateInfo, progress: Progress },
            FailPrepare,
            FailFetch { info: UpdateInfo, progress: Progress },
            FailStage { info: UpdateInfo, progress: Progress },
        }

        let state = DeState::deserialize(deserializer)?;

        let check_fields = |info, progress| {
            check_info_progress(info, progress).map_err(|e| D::Error::custom(e.to_string()))
        };

        Ok(match state {
            DeState::Prepare => State::Prepare,
            DeState::Fetch { info, progress } => {
                let () = check_fields(&info, &progress)?;
                State::Fetch { info, progress }
            }
            DeState::Stage { info, progress } => {
                let () = check_fields(&info, &progress)?;
                State::Stage { info, progress }
            }
            DeState::WaitToReboot { info, progress } => {
                let () = check_fields(&info, &progress)?;
                State::WaitToReboot { info, progress }
            }
            DeState::Reboot { info, progress } => {
                let () = check_fields(&info, &progress)?;
                State::Reboot { info, progress }
            }
            DeState::DeferReboot { info, progress } => {
                let () = check_fields(&info, &progress)?;
                State::DeferReboot { info, progress }
            }
            DeState::Complete { info, progress } => {
                let () = check_fields(&info, &progress)?;
                State::Complete { info, progress }
            }
            DeState::FailPrepare => State::FailPrepare,
            DeState::FailFetch { info, progress } => {
                let () = check_fields(&info, &progress)?;
                State::FailFetch { info, progress }
            }
            DeState::FailStage { info, progress } => {
                let () = check_fields(&info, &progress)?;
                State::FailStage { info, progress }
            }
        })
    }
}

impl<'de> Deserialize<'de> for Progress {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: serde::Deserializer<'de>,
    {
        #[derive(Debug, Deserialize)]
        pub struct DeProgress {
            fraction_completed: f32,
            bytes_downloaded: u64,
        }

        let progress = DeProgress::deserialize(deserializer)?;

        Ok(Progress::builder()
            .fraction_completed(progress.fraction_completed)
            .bytes_downloaded(progress.bytes_downloaded)
            .build())
    }
}

/// An error encountered while pairing an [`UpdateInfo`] and [`Progress`].
#[derive(Debug, Error, PartialEq, Eq)]
#[error("more bytes were fetched than should have been fetched")]
pub struct BytesFetchedExceedsDownloadSize;

fn check_info_progress(
    info: &UpdateInfo,
    progress: &Progress,
) -> Result<(), BytesFetchedExceedsDownloadSize> {
    if progress.bytes_downloaded > info.download_size {
        return Err(BytesFetchedExceedsDownloadSize);
    }

    Ok(())
}

/// An error encountered while decoding a [fidl_fuchsia_update_installer::State]
/// into a [State].
#[derive(Debug, Error, PartialEq, Eq)]
#[allow(missing_docs)]
pub enum DecodeStateError {
    #[error("missing field {0:?}")]
    MissingField(RequiredStateField),

    #[error("state contained invalid 'info' field")]
    DecodeUpdateInfo(#[source] DecodeUpdateInfoError),

    #[error("state contained invalid 'progress' field")]
    DecodeProgress(#[source] DecodeProgressError),

    #[error("the provided update info and progress are inconsistent with each other")]
    InconsistentUpdateInfoAndProgress(#[source] BytesFetchedExceedsDownloadSize),
}

/// Required fields in a [fidl_fuchsia_update_installer::State].
#[derive(Debug, PartialEq, Eq)]
#[allow(missing_docs)]
pub enum RequiredStateField {
    Info,
    Progress,
}

impl From<State> for fidl::State {
    fn from(state: State) -> Self {
        match state {
            State::Prepare => fidl::State::Prepare(fidl::PrepareData {}),
            State::Fetch { info, progress } => fidl::State::Fetch(fidl::FetchData {
                info: Some(info.into()),
                progress: Some(progress.into()),
            }),
            State::Stage { info, progress } => fidl::State::Stage(fidl::StageData {
                info: Some(info.into()),
                progress: Some(progress.into()),
            }),
            State::WaitToReboot { info, progress } => {
                fidl::State::WaitToReboot(fidl::WaitToRebootData {
                    info: Some(info.into()),
                    progress: Some(progress.into()),
                })
            }
            State::Reboot { info, progress } => fidl::State::Reboot(fidl::RebootData {
                info: Some(info.into()),
                progress: Some(progress.into()),
            }),
            State::DeferReboot { info, progress } => {
                fidl::State::DeferReboot(fidl::DeferRebootData {
                    info: Some(info.into()),
                    progress: Some(progress.into()),
                })
            }
            State::Complete { info, progress } => fidl::State::Complete(fidl::CompleteData {
                info: Some(info.into()),
                progress: Some(progress.into()),
            }),
            State::FailPrepare => fidl::State::FailPrepare(fidl::FailPrepareData {}),
            State::FailFetch { info, progress } => fidl::State::FailFetch(fidl::FailFetchData {
                info: Some(info.into()),
                progress: Some(progress.into()),
            }),
            State::FailStage { info, progress } => fidl::State::FailStage(fidl::FailStageData {
                info: Some(info.into()),
                progress: Some(progress.into()),
            }),
        }
    }
}

impl TryFrom<fidl::State> for State {
    type Error = DecodeStateError;

    fn try_from(state: fidl::State) -> Result<Self, Self::Error> {
        fn decode_info_progress(
            info: Option<fidl::UpdateInfo>,
            progress: Option<fidl::InstallationProgress>,
        ) -> Result<(UpdateInfo, Progress), DecodeStateError> {
            let info: UpdateInfo = info
                .ok_or(DecodeStateError::MissingField(RequiredStateField::Info))?
                .try_into()
                .map_err(DecodeStateError::DecodeUpdateInfo)?;
            let progress: Progress = progress
                .ok_or(DecodeStateError::MissingField(RequiredStateField::Progress))?
                .try_into()
                .map_err(DecodeStateError::DecodeProgress)?;

            let () = check_info_progress(&info, &progress)
                .map_err(DecodeStateError::InconsistentUpdateInfoAndProgress)?;

            Ok((info, progress))
        }

        Ok(match state {
            fidl::State::Prepare(fidl::PrepareData {}) => State::Prepare,
            fidl::State::Fetch(fidl::FetchData { info, progress }) => {
                let (info, progress) = decode_info_progress(info, progress)?;
                State::Fetch { info, progress }
            }
            fidl::State::Stage(fidl::StageData { info, progress }) => {
                let (info, progress) = decode_info_progress(info, progress)?;
                State::Stage { info, progress }
            }
            fidl::State::WaitToReboot(fidl::WaitToRebootData { info, progress }) => {
                let (info, progress) = decode_info_progress(info, progress)?;
                State::WaitToReboot { info, progress }
            }
            fidl::State::Reboot(fidl::RebootData { info, progress }) => {
                let (info, progress) = decode_info_progress(info, progress)?;
                State::Reboot { info, progress }
            }
            fidl::State::DeferReboot(fidl::DeferRebootData { info, progress }) => {
                let (info, progress) = decode_info_progress(info, progress)?;
                State::DeferReboot { info, progress }
            }
            fidl::State::Complete(fidl::CompleteData { info, progress }) => {
                let (info, progress) = decode_info_progress(info, progress)?;
                State::Complete { info, progress }
            }
            fidl::State::FailPrepare(fidl::FailPrepareData {}) => State::FailPrepare,
            fidl::State::FailFetch(fidl::FailFetchData { info, progress }) => {
                let (info, progress) = decode_info_progress(info, progress)?;
                State::FailFetch { info, progress }
            }
            fidl::State::FailStage(fidl::FailStageData { info, progress }) => {
                let (info, progress) = decode_info_progress(info, progress)?;
                State::FailStage { info, progress }
            }
        })
    }
}

// TODO remove ambiguous mapping of 0 to/from None when the system-updater actually computes a
// download size and emits bytes_downloaded information.
fn none_or_some_nonzero(n: u64) -> Option<u64> {
    if n == 0 {
        None
    } else {
        Some(n)
    }
}

/// An error encountered while decoding a [fidl_fuchsia_update_installer::UpdateInfo] into a
/// [UpdateInfo].
#[derive(Debug, Error, PartialEq, Eq)]
#[allow(missing_docs)]
pub enum DecodeUpdateInfoError {}

impl From<UpdateInfo> for fidl::UpdateInfo {
    fn from(info: UpdateInfo) -> Self {
        fidl::UpdateInfo { download_size: none_or_some_nonzero(info.download_size) }
    }
}

impl TryFrom<fidl::UpdateInfo> for UpdateInfo {
    type Error = DecodeUpdateInfoError;

    fn try_from(info: fidl::UpdateInfo) -> Result<Self, Self::Error> {
        Ok(UpdateInfo { download_size: info.download_size.unwrap_or(0) })
    }
}

/// An error encountered while decoding a [fidl_fuchsia_update_installer::InstallationProgress]
/// into a [Progress].
#[derive(Debug, Error, PartialEq, Eq)]
#[allow(missing_docs)]
pub enum DecodeProgressError {
    #[error("missing field {0:?}")]
    MissingField(RequiredProgressField),

    #[error("fraction completed not in range [0.0, 1.0]")]
    FractionCompletedOutOfRange,
}

/// Required fields in a [fidl_fuchsia_update_installer::InstallationProgress].
#[derive(Debug, PartialEq, Eq)]
#[allow(missing_docs)]
pub enum RequiredProgressField {
    FractionCompleted,
}

impl From<Progress> for fidl::InstallationProgress {
    fn from(progress: Progress) -> Self {
        fidl::InstallationProgress {
            fraction_completed: Some(progress.fraction_completed),
            bytes_downloaded: none_or_some_nonzero(progress.bytes_downloaded),
        }
    }
}

impl TryFrom<fidl::InstallationProgress> for Progress {
    type Error = DecodeProgressError;

    fn try_from(progress: fidl::InstallationProgress) -> Result<Self, Self::Error> {
        Ok(Progress {
            fraction_completed: {
                let n = progress.fraction_completed.ok_or(DecodeProgressError::MissingField(
                    RequiredProgressField::FractionCompleted,
                ))?;
                if n < 0.0 || n > 1.0 {
                    return Err(DecodeProgressError::FractionCompletedOutOfRange);
                }
                n
            },
            bytes_downloaded: progress.bytes_downloaded.unwrap_or(0),
        })
    }
}

/// Returns a strategy generating and UpdateInfo and Progress such that the Progress does not
/// exceed the bounds of the UpdateInfo.
fn arb_info_and_progress() -> impl Strategy<Value = (UpdateInfo, Progress)> {
    prop_compose! {
        fn arb_progress_for_info(
            info: UpdateInfo
        )(
            fraction_completed: f32,
            bytes_downloaded in 0..=info.download_size
        ) -> Progress {
            Progress::builder()
                .fraction_completed(fraction_completed)
                .bytes_downloaded(bytes_downloaded)
                .build()
        }
    }

    any::<UpdateInfo>().prop_flat_map(|info| (Just(info), arb_progress_for_info(info)))
}

fn arb_state_fetch() -> impl Strategy<Value = State> {
    arb_info_and_progress().prop_map(|(info, progress)| State::Fetch { info, progress })
}
fn arb_state_stage() -> impl Strategy<Value = State> {
    arb_info_and_progress().prop_map(|(info, progress)| State::Stage { info, progress })
}
fn arb_state_wait_to_reboot() -> impl Strategy<Value = State> {
    arb_info_and_progress().prop_map(|(info, progress)| State::WaitToReboot { info, progress })
}
fn arb_state_reboot() -> impl Strategy<Value = State> {
    arb_info_and_progress().prop_map(|(info, progress)| State::Reboot { info, progress })
}
fn arb_state_defer_reboot() -> impl Strategy<Value = State> {
    arb_info_and_progress().prop_map(|(info, progress)| State::DeferReboot { info, progress })
}
fn arb_state_complete() -> impl Strategy<Value = State> {
    arb_info_and_progress().prop_map(|(info, progress)| State::Complete { info, progress })
}
fn arb_state_fail_fetch() -> impl Strategy<Value = State> {
    arb_info_and_progress().prop_map(|(info, progress)| State::FailFetch { info, progress })
}
fn arb_state_fail_stage() -> impl Strategy<Value = State> {
    arb_info_and_progress().prop_map(|(info, progress)| State::Stage { info, progress })
}

#[cfg(test)]
mod tests {
    use {super::*, matches::assert_matches, serde_json::json};

    prop_compose! {
        fn arb_progress()(fraction_completed: f32, bytes_downloaded: u64) -> Progress {
            Progress::builder()
                .fraction_completed(fraction_completed)
                .bytes_downloaded(bytes_downloaded)
                .build()
        }
    }

    /// Returns a strategy generating (a, b) such that a < b.
    fn a_lt_b() -> impl Strategy<Value = (u64, u64)> {
        (0..u64::MAX).prop_flat_map(|a| (Just(a), a + 1..))
    }

    proptest! {
        #[test]
        fn progress_builder_clamps_fraction_completed(progress in arb_progress()) {
            prop_assert!(progress.fraction_completed() >= 0.0);
            prop_assert!(progress.fraction_completed() <= 1.0);
        }

        #[test]
        fn progress_builder_roundtrips(progress: Progress) {
            prop_assert_eq!(
                Progress::builder()
                    .fraction_completed(progress.fraction_completed())
                    .bytes_downloaded(progress.bytes_downloaded())
                    .build(),
                progress
            );
        }

        #[test]
        fn update_info_builder_roundtrips(info: UpdateInfo) {
            prop_assert_eq!(
                UpdateInfo::builder()
                    .download_size(info.download_size())
                    .build(),
                info
            );
        }

        #[test]
        fn update_info_roundtrips_through_fidl(info: UpdateInfo) {
            let as_fidl: fidl::UpdateInfo = info.clone().into();
            prop_assert_eq!(as_fidl.try_into(), Ok(info));
        }

        #[test]
        fn progress_roundtrips_through_fidl(progress: Progress) {
            let as_fidl: fidl::InstallationProgress = progress.clone().into();
            prop_assert_eq!(as_fidl.try_into(), Ok(progress));
        }

        #[test]
        fn state_roundtrips_through_fidl(state: State) {
            let as_fidl: fidl::State = state.clone().into();
            prop_assert_eq!(as_fidl.try_into(), Ok(state));
        }

        #[test]
        fn state_roundtrips_through_json(state: State) {
            let as_json = serde_json::to_value(&state).unwrap();
            let state2 = serde_json::from_value(as_json).unwrap();
            prop_assert_eq!(state, state2);
        }

        #[test]
        fn progress_rejects_invalid_fraction_completed(progress: Progress, fraction_completed: f32) {
            let fraction_valid = fraction_completed >= 0.0 && fraction_completed <= 1.0;
            prop_assume!(!fraction_valid);
            // Note, the above doesn't look simplified, but not all the usual math rules apply to
            // types that are PartialOrd and not Ord:
            //use std::f32::NAN;
            //assert!(!(NAN >= 0.0 && NAN <= 1.0)); // This assertion passes.
            //assert!(NAN < 0.0 || NAN > 1.0); // This assertion fails.

            let mut as_fidl: fidl::InstallationProgress = progress.into();
            as_fidl.fraction_completed = Some(fraction_completed);
            prop_assert_eq!(Progress::try_from(as_fidl), Err(DecodeProgressError::FractionCompletedOutOfRange));
        }

        #[test]
        fn state_rejects_too_many_bytes_fetched(state: State, (a, b) in a_lt_b()) {
            let mut as_fidl: fidl::State = state.into();

            let break_info_progress = |info: &mut Option<fidl::UpdateInfo>, progress: &mut Option<fidl::InstallationProgress>| {
                info.as_mut().unwrap().download_size = Some(a);
                progress.as_mut().unwrap().bytes_downloaded = Some(b);
            };

            match &mut as_fidl {
                fidl::State::Prepare(fidl::PrepareData {}) => prop_assume!(false),
                fidl::State::Fetch(fidl::FetchData { info, progress }) => break_info_progress(info, progress),
                fidl::State::Stage(fidl::StageData { info, progress }) => break_info_progress(info, progress),
                fidl::State::WaitToReboot(fidl::WaitToRebootData { info, progress }) => break_info_progress(info, progress),
                fidl::State::Reboot(fidl::RebootData { info, progress }) => break_info_progress(info, progress),
                fidl::State::DeferReboot(fidl::DeferRebootData { info, progress }) => break_info_progress(info, progress),
                fidl::State::Complete(fidl::CompleteData { info, progress }) => break_info_progress(info, progress),
                fidl::State::FailPrepare(fidl::FailPrepareData {}) => prop_assume!(false),
                fidl::State::FailFetch(fidl::FailFetchData { info, progress }) => break_info_progress(info, progress),
                fidl::State::FailStage(fidl::FailStageData { info, progress }) => break_info_progress(info, progress),
            }
            prop_assert_eq!(
                State::try_from(as_fidl),
                Err(DecodeStateError::InconsistentUpdateInfoAndProgress(BytesFetchedExceedsDownloadSize))
            );
        }
    }

    #[test]
    fn progress_fraction_completed_required() {
        assert_eq!(
            Progress::try_from(fidl::InstallationProgress::empty()),
            Err(DecodeProgressError::MissingField(RequiredProgressField::FractionCompleted)),
        );
    }

    #[test]
    fn json_deserializes_state() {
        assert_eq!(
            serde_json::from_value::<State>(json!({
                "id": "reboot",
                "info": {
                    "download_size": 100,
                },
                "progress": {
                    "bytes_downloaded": 100,
                    "fraction_completed": 1.0,
                },
            }))
            .unwrap(),
            State::Reboot {
                info: UpdateInfo { download_size: 100 },
                progress: Progress { bytes_downloaded: 100, fraction_completed: 1.0 },
            }
        );
    }

    #[test]
    fn json_deserialize_detects_inconsistent_info_and_progress() {
        let too_much_download = json!({
            "id": "reboot",
            "info": {
                "download_size": 100,
            },
            "progress": {
                "bytes_downloaded": 101,
                "fraction_completed": 1.0,
            },
        });

        assert_matches!(serde_json::from_value::<State>(too_much_download), Err(_));
    }

    #[test]
    fn json_deserialize_clamps_invalid_fraction_completed() {
        let too_much_progress = json!({
            "bytes_downloaded": 0,
            "fraction_completed": 1.1,
        });
        assert_eq!(
            serde_json::from_value::<Progress>(too_much_progress).unwrap(),
            Progress { bytes_downloaded: 0, fraction_completed: 1.0 }
        );

        let negative_progress = json!({
            "bytes_downloaded": 0,
            "fraction_completed": -0.5,
        });
        assert_eq!(
            serde_json::from_value::<Progress>(negative_progress).unwrap(),
            Progress { bytes_downloaded: 0, fraction_completed: 0.0 }
        );
    }
}
