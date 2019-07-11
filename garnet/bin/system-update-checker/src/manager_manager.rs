// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::apply::{apply_system_update, Initiator};
use crate::check::{check_for_system_update, SystemUpdateStatus};
use failure::{Error, ResultExt};
use fidl_fuchsia_update::{CheckStartedResult, ManagerState};
use fuchsia_async as fasync;
use fuchsia_merkle::Hash;
use fuchsia_syslog::{fx_log_err, fx_log_info};
use futures::future::BoxFuture;
use futures::prelude::*;
use parking_lot::Mutex;
use std::sync::Arc;

pub trait StateChangeCallback: Clone + Send + Sync + 'static {
    fn on_state_change(&self, new_state: State) -> Result<(), Error>;
}

#[derive(Clone)]
pub struct ManagerManager<C, A, S>
where
    C: UpdateChecker,
    A: UpdateApplier,
    S: StateChangeCallback,
{
    state: Arc<Mutex<ManagerManagerState<S>>>,
    update_checker: C,
    update_applier: A,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct State {
    pub manager_state: ManagerState,
    pub version_available: Option<String>,
}

impl Default for State {
    fn default() -> Self {
        Self { manager_state: ManagerState::Idle, version_available: None }
    }
}

impl<C, A, S> ManagerManager<C, A, S>
where
    C: UpdateChecker,
    A: UpdateApplier,
    S: StateChangeCallback,
    Self: Clone,
{
    pub fn from_checker_and_applier(update_checker: C, update_applier: A) -> Self {
        Self {
            state: Arc::new(Mutex::new(ManagerManagerState::new())),
            update_checker,
            update_applier,
        }
    }

    /// A Fuchsia Executor must be active when this method is called, b/c it uses fuchsia_async::spawn
    pub fn try_start_update(
        &self,
        initiator: Initiator,
        callback: Option<S>,
    ) -> CheckStartedResult {
        let mut state = self.state.lock();
        callback.map(|cb| state.add_temporary_callback(cb));
        match state.manager_state {
            ManagerState::Idle => {
                state.advance_manager_state(ManagerState::CheckingForUpdates);
                let manager_manager = (*self).clone();
                // Spawn so that callers of this method are not blocked
                fasync::spawn(async move {
                    await!(manager_manager.do_system_update_check_and_return_to_idle(initiator))
                });
                CheckStartedResult::Started
            }
            _ => CheckStartedResult::InProgress,
        }
    }

    pub fn get_state(&self) -> State {
        let state = self.state.lock();
        state.state()
    }

    pub fn add_permanent_callback(&self, callback: S) {
        self.state.lock().add_permanent_callback(callback);
    }

    async fn do_system_update_check_and_return_to_idle(&self, initiator: Initiator) {
        if let Err(e) = await!(self.do_system_update_check(initiator)) {
            fx_log_err!("update attempt failed: {}", e);
            self.state.lock().advance_manager_state(ManagerState::EncounteredError);
        }
        let mut state = self.state.lock();
        match state.manager_state {
            ManagerState::WaitingForReboot => fx_log_err!(
                "system-update-checker is in the WaitingForReboot state. \
                 This should not have happened, because the sytem-updater should \
                 have rebooted the device before it returned."
            ),
            _ => {
                state.advance_manager_state(ManagerState::Idle);
            }
        }
    }

    async fn do_system_update_check(&self, initiator: Initiator) -> Result<(), Error> {
        match await!(self.update_checker.check()).context("check_for_system_update failed")? {
            SystemUpdateStatus::UpToDate { system_image } => {
                fx_log_info!("current system_image merkle: {}", system_image);
                fx_log_info!("system_image is already up-to-date");
                return Ok(());
            }
            SystemUpdateStatus::UpdateAvailable { current_system_image, latest_system_image } => {
                fx_log_info!("current system_image merkle: {}", current_system_image);
                fx_log_info!("new system_image available: {}", latest_system_image);
                {
                    let mut state = self.state.lock();
                    state.version_available = Some(latest_system_image.to_string());
                    state.advance_manager_state(ManagerState::PerformingUpdate);
                }
                await!(self.update_applier.apply(
                    current_system_image,
                    latest_system_image,
                    initiator
                ))
                .context("apply_system_update failed")?;
                // On success, system-updater reboots the system before returning, so this code
                // should never run. The only way to leave WaitingForReboot state is to restart
                // the component
                self.state.lock().advance_manager_state(ManagerState::WaitingForReboot);
            }
        }
        Ok(())
    }
}

struct ManagerManagerState<S>
where
    S: StateChangeCallback,
{
    permanent_callbacks: Vec<S>,
    temporary_callbacks: Vec<S>,
    manager_state: ManagerState,
    version_available: Option<String>,
}

impl<S> ManagerManagerState<S>
where
    S: StateChangeCallback,
{
    fn new() -> Self {
        ManagerManagerState {
            permanent_callbacks: vec![],
            temporary_callbacks: vec![],
            manager_state: ManagerState::Idle,
            version_available: None,
        }
    }

    fn add_temporary_callback(&mut self, callback: S) {
        if callback.on_state_change(self.state()).is_ok() {
            self.temporary_callbacks.push(callback);
        }
    }

    fn add_permanent_callback(&mut self, callback: S) {
        if callback.on_state_change(self.state()).is_ok() {
            self.permanent_callbacks.push(callback);
        }
    }

    fn advance_manager_state(&mut self, next_manager_state: ManagerState) {
        self.manager_state = next_manager_state;
        if self.manager_state == ManagerState::Idle {
            self.version_available = None;
        }
        self.send_on_state();
        if self.manager_state == ManagerState::Idle {
            self.temporary_callbacks.clear();
        }
    }

    fn send_on_state(&mut self) {
        let state = self.state();
        self.permanent_callbacks.retain(|cb| cb.on_state_change(state.clone()).is_ok());
        self.temporary_callbacks.retain(|cb| cb.on_state_change(state.clone()).is_ok());
    }

    fn state(&self) -> State {
        State {
            manager_state: self.manager_state,
            version_available: self.version_available.clone(),
        }
    }
}

// For mocking
pub trait UpdateChecker: Clone + Send + Sync + 'static {
    fn check(&self) -> BoxFuture<Result<SystemUpdateStatus, crate::errors::Error>>;
}

#[derive(Clone, Copy)]
pub struct RealUpdateChecker;

impl UpdateChecker for RealUpdateChecker {
    fn check(&self) -> BoxFuture<Result<SystemUpdateStatus, crate::errors::Error>> {
        check_for_system_update().boxed()
    }
}

// For mocking
pub trait UpdateApplier: Clone + Send + Sync + 'static {
    fn apply(
        &self,
        current_system_image: Hash,
        latest_system_image: Hash,
        initiator: Initiator,
    ) -> BoxFuture<Result<(), crate::errors::Error>>;
}

#[derive(Clone, Copy)]
pub struct RealUpdateApplier;

impl UpdateApplier for RealUpdateApplier {
    fn apply(
        &self,
        current_system_image: Hash,
        latest_system_image: Hash,
        initiator: Initiator,
    ) -> BoxFuture<Result<(), crate::errors::Error>> {
        apply_system_update(current_system_image, latest_system_image, initiator).boxed()
    }
}

#[cfg(test)]
pub(crate) mod test {
    use super::*;
    use futures::channel::mpsc::{channel, Receiver, Sender};
    use futures::channel::oneshot;
    use matches::assert_matches;
    use std::sync::atomic::AtomicU64;

    pub const CALLBACK_CHANNEL_SIZE: usize = 20;
    pub const CURRENT_SYSTEM_IMAGE: &str =
        "0000000000000000000000000000000000000000000000000000000000000000";
    pub const LATEST_SYSTEM_IMAGE: &str =
        "1111111111111111111111111111111111111111111111111111111111111111";

    #[derive(Clone)]
    pub struct FakeUpdateChecker {
        result: Result<SystemUpdateStatus, crate::errors::ErrorKind>,
    }
    impl FakeUpdateChecker {
        pub fn new_up_to_date() -> Self {
            Self {
                result: Ok(SystemUpdateStatus::UpToDate {
                    system_image: CURRENT_SYSTEM_IMAGE.parse().expect("valid merkle"),
                }),
            }
        }
        pub fn new_update_available() -> Self {
            Self {
                result: Ok(SystemUpdateStatus::UpdateAvailable {
                    current_system_image: CURRENT_SYSTEM_IMAGE.parse().expect("valid merkle"),
                    latest_system_image: LATEST_SYSTEM_IMAGE.parse().expect("valid merkle"),
                }),
            }
        }
        pub fn new_error() -> Self {
            Self { result: Err(crate::errors::ErrorKind::ResolveUpdatePackage) }
        }
    }
    impl UpdateChecker for FakeUpdateChecker {
        fn check(&self) -> BoxFuture<Result<SystemUpdateStatus, crate::errors::Error>> {
            future::ready(self.result.clone().map_err(|e| e.into())).boxed()
        }
    }

    #[derive(Clone)]
    pub struct FakeUpdateApplier {
        result: Result<(), crate::errors::ErrorKind>,
        call_count: Arc<AtomicU64>,
    }
    impl FakeUpdateApplier {
        pub fn new_success() -> Self {
            Self { result: Ok(()), call_count: Arc::new(AtomicU64::new(0)) }
        }
        pub fn new_error() -> Self {
            Self {
                result: Err(crate::errors::ErrorKind::SystemUpdaterFailed),
                call_count: Arc::new(AtomicU64::new(0)),
            }
        }
        pub fn call_count(&self) -> u64 {
            self.call_count.load(std::sync::atomic::Ordering::Relaxed)
        }
    }
    impl UpdateApplier for FakeUpdateApplier {
        fn apply(
            &self,
            _current_system_image: Hash,
            _latest_system_image: Hash,
            _initiator: Initiator,
        ) -> BoxFuture<Result<(), crate::errors::Error>> {
            self.call_count.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
            future::ready(self.result.clone().map_err(|e| e.into())).boxed()
        }
    }

    #[derive(Clone)]
    struct FakeStateChangeCallback {
        sender: Arc<Mutex<Sender<State>>>,
    }
    impl FakeStateChangeCallback {
        fn new_callback_and_receiver() -> (Self, Receiver<State>) {
            let (sender, receiver) = channel(CALLBACK_CHANNEL_SIZE);
            (Self { sender: Arc::new(Mutex::new(sender)) }, receiver)
        }
    }
    impl StateChangeCallback for FakeStateChangeCallback {
        fn on_state_change(&self, new_state: State) -> Result<(), Error> {
            self.sender
                .lock()
                .try_send(new_state)
                .expect("FakeStateChangeCallback failed to send state");
            Ok(())
        }
    }

    type FakeManagerManager =
        ManagerManager<FakeUpdateChecker, FakeUpdateApplier, FakeStateChangeCallback>;

    type BlockingManagerManager =
        ManagerManager<BlockingUpdateChecker, FakeUpdateApplier, FakeStateChangeCallback>;

    async fn next_n_states(receiver: &mut Receiver<State>, n: usize) -> Vec<State> {
        let mut v = Vec::with_capacity(n);
        for _ in 0..n {
            v.push(await!(receiver.next()).expect("next_n_states stream empty"));
        }
        v
    }

    async fn wait_until_manager_state_n(
        receiver: &mut Receiver<State>,
        manager_state: ManagerState,
        mut seen_count: u64,
    ) {
        if seen_count == 0 {
            return;
        }
        while let Some(new_state) = await!(receiver.next()) {
            if new_state.manager_state == manager_state {
                seen_count -= 1;
                if seen_count == 0 {
                    return;
                }
            }
        }
        panic!("wait_until_state_n emptied stream: {}", seen_count);
    }

    impl From<ManagerState> for State {
        fn from(manager_state: ManagerState) -> Self {
            match manager_state {
                ManagerState::Idle | ManagerState::CheckingForUpdates => {
                    State { manager_state, version_available: None }
                }
                manager_state => State {
                    manager_state,
                    version_available: Some(LATEST_SYSTEM_IMAGE.to_string()),
                },
            }
        }
    }

    #[test]
    fn test_correct_initial_state() {
        let manager = FakeManagerManager::from_checker_and_applier(
            FakeUpdateChecker::new_up_to_date(),
            FakeUpdateApplier::new_success(),
        );

        assert_eq!(manager.get_state(), Default::default());
    }

    #[test]
    fn test_try_start_update_returns_started() {
        let _executor = fasync::Executor::new().expect("create test executor");
        let manager = FakeManagerManager::from_checker_and_applier(
            FakeUpdateChecker::new_up_to_date(),
            FakeUpdateApplier::new_success(),
        );

        assert_eq!(manager.try_start_update(Initiator::Manual, None), CheckStartedResult::Started);
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_temporary_callbacks_dropped_after_update_attempt() {
        let manager = FakeManagerManager::from_checker_and_applier(
            FakeUpdateChecker::new_up_to_date(),
            FakeUpdateApplier::new_success(),
        );
        let (callback0, mut receiver0) = FakeStateChangeCallback::new_callback_and_receiver();
        let (callback1, mut receiver1) = FakeStateChangeCallback::new_callback_and_receiver();

        manager.try_start_update(Initiator::Manual, Some(callback0));

        // Wait for first update attempt to complete, to guarantee that the second
        // try_start_update() call starts a new attempt (and generates more callback calls).
        await!(wait_until_manager_state_n(&mut receiver0, ManagerState::Idle, 2));

        manager.try_start_update(Initiator::Manual, Some(callback1));

        // Wait for the second update attempt to complete, to guarantee the callbacks
        // have been called with more states.
        await!(wait_until_manager_state_n(&mut receiver1, ManagerState::Idle, 2));

        // The first callback should have been dropped after the first attempt completed,
        // so it should still be empty.
        assert_matches!(receiver0.try_next(), Ok(None));
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_try_start_update_callback_when_up_to_date() {
        let manager = FakeManagerManager::from_checker_and_applier(
            FakeUpdateChecker::new_up_to_date(),
            FakeUpdateApplier::new_success(),
        );
        let (callback, receiver) = FakeStateChangeCallback::new_callback_and_receiver();

        manager.try_start_update(Initiator::Manual, Some(callback));

        assert_eq!(
            await!(receiver.collect::<Vec<State>>()),
            vec![
                ManagerState::Idle.into(),
                ManagerState::CheckingForUpdates.into(),
                ManagerState::Idle.into()
            ]
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_try_start_update_callback_when_update_available_and_apply_errors() {
        let manager = FakeManagerManager::from_checker_and_applier(
            FakeUpdateChecker::new_update_available(),
            FakeUpdateApplier::new_error(),
        );
        let (callback, receiver) = FakeStateChangeCallback::new_callback_and_receiver();

        manager.try_start_update(Initiator::Manual, Some(callback));

        assert_eq!(
            await!(receiver.collect::<Vec<State>>()),
            vec![
                ManagerState::Idle.into(),
                ManagerState::CheckingForUpdates.into(),
                ManagerState::PerformingUpdate.into(),
                ManagerState::EncounteredError.into(),
                ManagerState::Idle.into()
            ]
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_try_start_update_callback_when_update_available_and_apply_succeeds() {
        let manager = FakeManagerManager::from_checker_and_applier(
            FakeUpdateChecker::new_update_available(),
            FakeUpdateApplier::new_success(),
        );
        let (callback, mut receiver) = FakeStateChangeCallback::new_callback_and_receiver();

        manager.try_start_update(Initiator::Manual, Some(callback));

        assert_eq!(
            await!(next_n_states(&mut receiver, 4)),
            vec![
                ManagerState::Idle.into(),
                ManagerState::CheckingForUpdates.into(),
                ManagerState::PerformingUpdate.into(),
                ManagerState::WaitingForReboot.into(),
            ]
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_permanent_callback_is_called() {
        let manager = FakeManagerManager::from_checker_and_applier(
            FakeUpdateChecker::new_update_available(),
            FakeUpdateApplier::new_error(),
        );
        let (callback, mut receiver) = FakeStateChangeCallback::new_callback_and_receiver();

        manager.add_permanent_callback(callback);
        manager.try_start_update(Initiator::Manual, None);

        assert_eq!(
            await!(next_n_states(&mut receiver, 5)),
            vec![
                ManagerState::Idle.into(),
                ManagerState::CheckingForUpdates.into(),
                ManagerState::PerformingUpdate.into(),
                ManagerState::EncounteredError.into(),
                ManagerState::Idle.into(),
            ]
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_permanent_callback_persists_across_attempts() {
        let manager = FakeManagerManager::from_checker_and_applier(
            FakeUpdateChecker::new_update_available(),
            FakeUpdateApplier::new_error(),
        );
        let (callback, mut receiver) = FakeStateChangeCallback::new_callback_and_receiver();

        manager.add_permanent_callback(callback);
        manager.try_start_update(Initiator::Manual, None);

        // waiting for Idle state guarantees second try_start_update call
        // starts a new attempt
        assert_eq!(
            await!(next_n_states(&mut receiver, 5)),
            vec![
                ManagerState::Idle.into(),
                ManagerState::CheckingForUpdates.into(),
                ManagerState::PerformingUpdate.into(),
                ManagerState::EncounteredError.into(),
                ManagerState::Idle.into(),
            ]
        );

        manager.try_start_update(Initiator::Manual, None);

        assert_eq!(
            await!(next_n_states(&mut receiver, 4)),
            vec![
                ManagerState::CheckingForUpdates.into(),
                ManagerState::PerformingUpdate.into(),
                ManagerState::EncounteredError.into(),
                ManagerState::Idle.into(),
            ]
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_update_applier_called_if_update_available() {
        let update_applier = FakeUpdateApplier::new_error();
        let manager = FakeManagerManager::from_checker_and_applier(
            FakeUpdateChecker::new_update_available(),
            update_applier.clone(),
        );
        let (callback, receiver) = FakeStateChangeCallback::new_callback_and_receiver();

        manager.try_start_update(Initiator::Manual, Some(callback));
        await!(receiver.collect::<Vec<State>>());

        assert_eq!(update_applier.call_count(), 1);
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_update_applier_not_called_if_up_to_date() {
        let update_applier = FakeUpdateApplier::new_error();
        let manager = FakeManagerManager::from_checker_and_applier(
            FakeUpdateChecker::new_up_to_date(),
            update_applier.clone(),
        );
        let (callback, receiver) = FakeStateChangeCallback::new_callback_and_receiver();

        manager.try_start_update(Initiator::Manual, Some(callback));
        await!(receiver.collect::<Vec<State>>());

        assert_eq!(update_applier.call_count(), 0);
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_return_to_initial_state_on_update_check_error() {
        let manager = FakeManagerManager::from_checker_and_applier(
            FakeUpdateChecker::new_error(),
            FakeUpdateApplier::new_error(),
        );
        let (callback, receiver) = FakeStateChangeCallback::new_callback_and_receiver();

        manager.try_start_update(Initiator::Manual, Some(callback));
        await!(receiver.collect::<Vec<State>>());

        assert_eq!(manager.get_state(), Default::default());
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_return_to_initial_state_on_update_apply_error() {
        let manager = FakeManagerManager::from_checker_and_applier(
            FakeUpdateChecker::new_update_available(),
            FakeUpdateApplier::new_error(),
        );
        let (callback, receiver) = FakeStateChangeCallback::new_callback_and_receiver();

        manager.try_start_update(Initiator::Manual, Some(callback));
        await!(receiver.collect::<Vec<State>>());

        assert_eq!(manager.get_state(), Default::default());
    }

    #[derive(Clone)]
    pub struct BlockingUpdateChecker {
        blocker: future::Shared<oneshot::Receiver<()>>,
    }
    impl BlockingUpdateChecker {
        pub fn new_checker_and_sender() -> (Self, oneshot::Sender<()>) {
            let (sender, receiver) = oneshot::channel();
            let blocking_update_checker = BlockingUpdateChecker { blocker: receiver.shared() };
            (blocking_update_checker, sender)
        }
    }
    impl UpdateChecker for BlockingUpdateChecker {
        fn check(&self) -> BoxFuture<Result<SystemUpdateStatus, crate::errors::Error>> {
            let blocker = self.blocker.clone();
            async move {
                assert!(await!(blocker).is_ok(), "blocking future cancelled");
                Ok(SystemUpdateStatus::UpdateAvailable {
                    current_system_image: CURRENT_SYSTEM_IMAGE.parse().expect("valid merkle"),
                    latest_system_image: LATEST_SYSTEM_IMAGE.parse().expect("valid merkle"),
                })
            }
                .boxed()
        }
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_get_state_in_checking_for_updates() {
        let (blocking_update_checker, _sender) = BlockingUpdateChecker::new_checker_and_sender();
        let manager = BlockingManagerManager::from_checker_and_applier(
            blocking_update_checker,
            FakeUpdateApplier::new_error(),
        );

        manager.try_start_update(Initiator::Manual, None);

        assert_eq!(manager.get_state().manager_state, ManagerState::CheckingForUpdates);
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_no_concurrent_update_attempts() {
        let (blocking_update_checker, sender) = BlockingUpdateChecker::new_checker_and_sender();
        let update_applier = FakeUpdateApplier::new_error();
        let manager = BlockingManagerManager::from_checker_and_applier(
            blocking_update_checker,
            update_applier.clone(),
        );
        let (callback, receiver) = FakeStateChangeCallback::new_callback_and_receiver();

        let res0 = manager.try_start_update(Initiator::Manual, Some(callback));
        // try_start_update advances state to CheckingForUpdates before returning
        // and the blocking_update_checker keeps it there
        let res1 = manager.try_start_update(Initiator::Manual, None);
        assert_matches!(sender.send(()), Ok(()));
        await!(receiver.collect::<Vec<State>>());

        assert_eq!(res0, CheckStartedResult::Started);
        assert_eq!(res1, CheckStartedResult::InProgress);
        assert_eq!(update_applier.call_count(), 1);
    }
}
