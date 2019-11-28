// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// TODO Follow 2018 idioms
#![allow(elided_lifetimes_in_paths)]

//! This module contains the core algorithm for `WorkScheduler`, a component manager subsytem for
//! dispatching batches of work.
//!
//! The subsystem's interface consists of the following three FIDL prototocols:
//!
//! * `fuchsia.sys2.WorkScheduler`: A framework service for scheduling and canceling work.
//! * `fuchsia.sys2.Worker`: A service that `WorkScheduler` clients expose to the framework to be
//!   notified when work units are dispatched.
//! * `fuchsia.sys2.WorkSchedulerControl`: A built-in service for controlling the period between
//!   wakeup, batch, and dispatch cycles.

use {
    crate::{
        model::{OutgoingBinder, Realm},
        work_scheduler::{delegate::WorkSchedulerDelegate, dispatcher::RealDispatcher},
    },
    cm_rust::CapabilityPath,
    fidl_fuchsia_sys2 as fsys,
    futures::lock::Mutex,
    lazy_static::lazy_static,
    std::{
        convert::TryInto,
        sync::{Arc, Weak},
    },
};

// If you change this block, please update test `work_scheduler_capability_paths`.
lazy_static! {
    pub static ref WORKER_CAPABILITY_PATH: CapabilityPath =
        "/svc/fuchsia.sys2.Worker".try_into().unwrap();
    pub static ref WORK_SCHEDULER_CAPABILITY_PATH: CapabilityPath =
        "/svc/fuchsia.sys2.WorkScheduler".try_into().unwrap();
    pub static ref WORK_SCHEDULER_CONTROL_CAPABILITY_PATH: CapabilityPath =
        "/svc/fuchsia.sys2.WorkSchedulerControl".try_into().unwrap();
}

/// Owns the `Mutex`-synchronized `WorkSchedulerDelegate`, which contains business logic and state
/// for the `WorkScheduler` instance.
pub struct WorkScheduler {
    /// Delegate that implements business logic and holds state behind `Mutex`.
    delegate: Mutex<WorkSchedulerDelegate>,
    /// A reference to the `Model` used to bind to component instances during dispatch.
    outgoing_binder: Weak<dyn OutgoingBinder>,
}

impl WorkScheduler {
    // `Workscheduler` is always instantiated in an `Arc` that will determine its lifetime.
    pub async fn new(outgoing_binder: Weak<dyn OutgoingBinder>) -> Arc<Self> {
        let work_scheduler = Self::new_raw(outgoing_binder);
        {
            let mut delegate = work_scheduler.delegate.lock().await;
            delegate.init(Arc::downgrade(&work_scheduler));
        }
        work_scheduler
    }

    fn new_raw(outgoing_binder: Weak<dyn OutgoingBinder>) -> Arc<Self> {
        Arc::new(Self { delegate: WorkSchedulerDelegate::new(), outgoing_binder })
    }

    /// `schedule_work()` interface method is forwarded to delegate. `Arc<dyn Dispatcher>` is
    /// constructed late to keep it out of the public interface to `WorkScheduler`.
    pub async fn schedule_work<'a>(
        &'a self,
        realm: Arc<Realm>,
        work_id: &'a str,
        work_request: &'a fsys::WorkRequest,
    ) -> Result<(), fsys::Error> {
        let mut delegate = self.delegate.lock().await;
        delegate.schedule_work(
            RealDispatcher::new(realm, self.outgoing_binder.clone()),
            work_id,
            work_request,
        )
    }

    /// `cancel_work()` interface method is forwarded to delegate. `Arc<dyn Dispatcher>` is
    /// constructed late to keep it out of the public interface to `WorkScheduler`.
    pub async fn cancel_work<'a>(
        &'a self,
        realm: Arc<Realm>,
        work_id: &'a str,
    ) -> Result<(), fsys::Error> {
        let mut delegate = self.delegate.lock().await;
        delegate.cancel_work(RealDispatcher::new(realm, self.outgoing_binder.clone()), work_id)
    }

    /// `get_batch_period()` interface method is forwarded to delegate.
    pub async fn get_batch_period(&self) -> Result<i64, fsys::Error> {
        let delegate = self.delegate.lock().await;
        delegate.get_batch_period()
    }

    /// `set_batch_period()` interface method is forwarded to delegate.
    pub async fn set_batch_period(&self, batch_period: i64) -> Result<(), fsys::Error> {
        let mut delegate = self.delegate.lock().await;
        delegate.set_batch_period(batch_period)
    }

    /// `dispatch_work()` helper method is forwarded to delegate, `weak_self` in injected to allow
    /// internal timer to asynchronously call back into `dispatch_work()`.
    pub(super) async fn dispatch_work(&self) {
        let mut delegate = self.delegate.lock().await;
        delegate.dispatch_work()
    }
}

#[cfg(test)]
use crate::work_scheduler::work_item::WorkItem;

// Provide test-only access to schedulable `WorkItem` instances.
#[cfg(test)]
impl WorkScheduler {
    pub(super) async fn work_items(&self) -> Vec<WorkItem> {
        let delegate = self.delegate.lock().await;
        delegate.work_items().clone()
    }
}

#[cfg(test)]
mod path_tests {
    use {
        super::{
            WORKER_CAPABILITY_PATH, WORK_SCHEDULER_CAPABILITY_PATH,
            WORK_SCHEDULER_CONTROL_CAPABILITY_PATH,
        },
        fidl::endpoints::ServiceMarker,
        fidl_fuchsia_sys2 as fsys,
    };

    #[test]
    fn work_scheduler_capability_paths() {
        assert_eq!(
            format!("/svc/{}", fsys::WorkerMarker::NAME),
            WORKER_CAPABILITY_PATH.to_string()
        );
        assert_eq!(
            format!("/svc/{}", fsys::WorkSchedulerMarker::NAME),
            WORK_SCHEDULER_CAPABILITY_PATH.to_string()
        );
        assert_eq!(
            format!("/svc/{}", fsys::WorkSchedulerControlMarker::NAME),
            WORK_SCHEDULER_CONTROL_CAPABILITY_PATH.to_string()
        );
    }
}

#[cfg(test)]
mod time_tests {
    use {
        super::WorkScheduler,
        crate::{
            model::{testing::mocks::FakeOutgoingBinder, AbsoluteMoniker, OutgoingBinder},
            work_scheduler::work_item::WorkItem,
        },
        fidl_fuchsia_sys2 as fsys,
        fuchsia_async::{Executor, Time, WaitState},
        futures::{executor::block_on, Future},
        std::sync::Arc,
    };

    impl WorkScheduler {
        async fn schedule_work_item<'a>(
            &'a self,
            abs_moniker: &AbsoluteMoniker,
            work_id: &'a str,
            work_request: &'a fsys::WorkRequest,
        ) -> Result<(), fsys::Error> {
            let mut delegate = self.delegate.lock().await;
            delegate.schedule_work(Arc::new(abs_moniker.clone()), work_id, work_request)
        }
    }

    struct TestWorkUnit {
        start: i64,
        work_item: WorkItem,
    }

    impl TestWorkUnit {
        fn new(
            start: i64,
            abs_moniker: &AbsoluteMoniker,
            id: &str,
            next_deadline_monotonic: i64,
            period: Option<i64>,
        ) -> Self {
            TestWorkUnit {
                start,
                work_item: WorkItem::new(
                    Arc::new(abs_moniker.clone()),
                    id,
                    next_deadline_monotonic,
                    period,
                ),
            }
        }
    }

    struct TimeTest {
        executor: Executor,
        work_scheduler: Arc<WorkScheduler>,
        // Retain `Arc` to keep `OutgoingBinder` alive throughout test.
        _outgoing_binder: Arc<dyn OutgoingBinder>,
    }

    impl TimeTest {
        fn new() -> Self {
            let executor = Executor::new_with_fake_time().unwrap();
            executor.set_fake_time(Time::from_nanos(0));
            let _outgoing_binder = FakeOutgoingBinder::new();
            let work_scheduler = WorkScheduler::new_raw(Arc::downgrade(&_outgoing_binder));
            block_on(async {
                let mut delegate = work_scheduler.delegate.lock().await;
                delegate.init(Arc::downgrade(&work_scheduler));
            });
            TimeTest { executor, work_scheduler, _outgoing_binder }
        }

        fn work_scheduler(&self) -> Arc<WorkScheduler> {
            self.work_scheduler.clone()
        }

        fn set_time(&mut self, time: i64) {
            self.executor.set_fake_time(Time::from_nanos(time));
        }

        fn run_and_sync<F>(&mut self, fut: &mut F)
        where
            F: Future + Unpin,
        {
            assert!(self.executor.run_until_stalled(fut).is_ready());
            while self.executor.is_waiting() == WaitState::Ready {
                assert!(self.executor.run_until_stalled(&mut Box::pin(async {})).is_ready());
            }
        }

        fn set_time_and_run_timers(&mut self, time: i64) {
            self.set_time(time);
            assert!(self.executor.wake_expired_timers());
            while self.executor.is_waiting() == WaitState::Ready {
                assert!(self.executor.run_until_stalled(&mut Box::pin(async {})).is_ready());
            }
        }

        fn assert_no_timers(&mut self) {
            assert_eq!(None, self.executor.wake_next_timer());
        }

        fn assert_next_timer_at(&mut self, time: i64) {
            assert_eq!(WaitState::Waiting(Time::from_nanos(time)), self.executor.is_waiting());
        }

        fn assert_work_items(
            &mut self,
            work_scheduler: &Arc<WorkScheduler>,
            test_work_units: Vec<TestWorkUnit>,
        ) {
            self.run_and_sync(&mut Box::pin(async {
                // Check collection of work items.
                let work_items: Vec<WorkItem> = test_work_units
                    .iter()
                    .map(|test_work_unit| test_work_unit.work_item.clone())
                    .collect();
                assert_eq!(work_items, work_scheduler.work_items().await);

                // Check invariants on relationships between `now` and `WorkItem` state.
                let now = Time::now().into_nanos();
                for test_work_unit in test_work_units.iter() {
                    let work_item = &test_work_unit.work_item;
                    let deadline = work_item.next_deadline_monotonic;
                    // Either:
                    // 1. This is a check for initial state, in which case allow now=deadline=0, or
                    // 2. All deadlines should be in the future.
                    assert!(
                        (now == 0 && deadline == now) || now < deadline,
                        "Expected either
                            1. This is a check for initial state, so allow now=deadline=0, or
                            2. All deadlines should be in the future."
                    );
                    if let Some(period) = work_item.period {
                        // All periodic deadlines should be either:
                        // 1. Waiting to be dispatched for the first time, or
                        // 2. At most one period into the future (for otherwise, a period would be
                        //    skipped).
                        assert!(
                            now < test_work_unit.start || now + period >= deadline,
                            "Expected all periodic deadlines should be either:
                                1. Waiting to be dispatched for the first time, or
                                2. At most one period into the future (for otherwise, a period would
                                    be skipped"
                        );
                        // All periodic deadlines should be aligned to:
                        // `deadline = start + n*period` for some non-negative integer, `n`.
                        assert_eq!(
                            0,
                            (deadline - test_work_unit.start) % period,
                            "Expected all periodic deadlines should be aligned to:
                                `deadline = start + n*period` for some non-negative integer, `n`."
                        );
                    }
                }
            }));
        }

        fn assert_no_work(&mut self, work_scheduler: &Arc<WorkScheduler>) {
            self.run_and_sync(&mut Box::pin(async {
                assert_eq!(vec![] as Vec<WorkItem>, work_scheduler.work_items().await);
            }));
        }
    }

    #[test]
    fn work_scheduler_time_get_batch_period_queues_nothing() {
        let mut t = TimeTest::new();
        let work_scheduler = t.work_scheduler();
        t.run_and_sync(&mut Box::pin(async {
            assert_eq!(Ok(std::i64::MAX), work_scheduler.get_batch_period().await);
        }));
        t.assert_no_timers();
    }

    #[test]
    fn work_scheduler_time_set_batch_period_no_work_queues_nothing() {
        let mut t = TimeTest::new();
        let work_scheduler = t.work_scheduler();
        t.run_and_sync(&mut Box::pin(async {
            assert_eq!(Ok(()), work_scheduler.set_batch_period(1).await);
        }));
        t.assert_no_timers();
    }

    #[test]
    fn work_scheduler_time_schedule_inf_batch_period_queues_nothing() {
        let mut t = TimeTest::new();
        let work_scheduler = t.work_scheduler();
        t.run_and_sync(&mut Box::pin(async {
            let root = AbsoluteMoniker::root();
            let now_once =
                fsys::WorkRequest { start: Some(fsys::Start::MonotonicTime(0)), period: None };
            assert_eq!(
                Ok(()),
                work_scheduler.schedule_work_item(&root, "NOW_ONCE", &now_once).await
            );
        }));
        t.assert_no_timers();
    }

    #[test]
    fn work_scheduler_time_schedule_finite_batch_period_queues_and_dispatches() {
        let mut t = TimeTest::new();
        let work_scheduler = t.work_scheduler();
        let root = AbsoluteMoniker::root();

        // Set batch period and queue a unit of work.
        t.run_and_sync(&mut Box::pin(async {
            assert_eq!(Ok(()), work_scheduler.set_batch_period(1).await);
            let now_once =
                fsys::WorkRequest { start: Some(fsys::Start::MonotonicTime(0)), period: None };
            assert_eq!(
                Ok(()),
                work_scheduler.schedule_work_item(&root, "NOW_ONCE", &now_once).await
            );
        }));

        // Confirm timer and work item.
        t.assert_next_timer_at(1);
        t.assert_work_items(
            &work_scheduler,
            vec![TestWorkUnit::new(0, &root, "NOW_ONCE", 0, None)],
        );

        // Run work stemming from timer and confirm no more work items.
        t.set_time_and_run_timers(1);
        t.assert_no_work(&work_scheduler);
    }

    #[test]
    fn work_scheduler_time_periodic_stays_queued() {
        let mut t = TimeTest::new();
        let work_scheduler = t.work_scheduler();
        let root = AbsoluteMoniker::root();

        // Set batch period and queue a unit of work.
        t.run_and_sync(&mut Box::pin(async {
            assert_eq!(Ok(()), work_scheduler.set_batch_period(1).await);
            let every_moment =
                fsys::WorkRequest { start: Some(fsys::Start::MonotonicTime(0)), period: Some(1) };
            assert_eq!(
                Ok(()),
                work_scheduler.schedule_work_item(&root, "EVERY_MOMENT", &every_moment).await
            );
        }));

        // Confirm timer and work item.
        t.assert_next_timer_at(1);
        t.assert_work_items(
            &work_scheduler,
            vec![TestWorkUnit::new(0, &root, "EVERY_MOMENT", 0, Some(1))],
        );

        // Dispatch work and assert next periodic work item and timer.
        t.set_time_and_run_timers(1);
        t.assert_work_items(
            &work_scheduler,
            vec![TestWorkUnit::new(0, &root, "EVERY_MOMENT", 2, Some(1))],
        );
        t.assert_next_timer_at(3);
    }

    #[test]
    fn work_scheduler_time_timeout_updates_when_earlier_work_item_added() {
        let mut t = TimeTest::new();
        let work_scheduler = t.work_scheduler();
        let root = AbsoluteMoniker::root();

        // Set batch period and queue a unit of work.
        t.run_and_sync(&mut Box::pin(async {
            assert_eq!(Ok(()), work_scheduler.set_batch_period(5).await);
            let at_nine =
                fsys::WorkRequest { start: Some(fsys::Start::MonotonicTime(9)), period: None };
            assert_eq!(Ok(()), work_scheduler.schedule_work_item(&root, "AT_NINE", &at_nine).await);
        }));

        // Confirm timer and work item.
        t.assert_next_timer_at(10);
        t.assert_work_items(&work_scheduler, vec![TestWorkUnit::new(9, &root, "AT_NINE", 9, None)]);

        // Queue unit of work with deadline _earlier_ than first unit of work.
        t.run_and_sync(&mut Box::pin(async {
            let at_four =
                fsys::WorkRequest { start: Some(fsys::Start::MonotonicTime(4)), period: None };
            assert_eq!(Ok(()), work_scheduler.schedule_work_item(&root, "AT_FOUR", &at_four).await);
        }));

        // Confirm timer moved _back_, and work units are as expected.
        t.assert_next_timer_at(5);
        t.assert_work_items(
            &work_scheduler,
            vec![
                TestWorkUnit::new(4, &root, "AT_FOUR", 4, None),
                TestWorkUnit::new(9, &root, "AT_NINE", 9, None),
            ],
        );

        // Dispatch work and assert remaining work and timer.
        t.set_time_and_run_timers(5);
        t.assert_work_items(&work_scheduler, vec![TestWorkUnit::new(9, &root, "AT_NINE", 9, None)]);
        t.assert_next_timer_at(10);

        // Queue unit of work with deadline _later_ than existing unit of work.
        t.run_and_sync(&mut Box::pin(async {
            let at_ten =
                fsys::WorkRequest { start: Some(fsys::Start::MonotonicTime(10)), period: None };
            assert_eq!(Ok(()), work_scheduler.schedule_work_item(&root, "AT_TEN", &at_ten).await);
        }));

        // Confirm unchanged, and work units are as expected.
        t.assert_next_timer_at(10);
        t.assert_work_items(
            &work_scheduler,
            vec![
                TestWorkUnit::new(9, &root, "AT_NINE", 9, None),
                TestWorkUnit::new(10, &root, "AT_TEN", 10, None),
            ],
        );

        // Dispatch work and assert no work left.
        t.set_time_and_run_timers(10);
        t.assert_no_work(&work_scheduler);
    }

    #[test]
    fn work_scheduler_time_late_timer_fire() {
        let mut t = TimeTest::new();
        let work_scheduler = t.work_scheduler();
        let root = AbsoluteMoniker::root();

        // Set period and schedule two work items, one of which _should_ be dispatched in a second
        // cycle.
        t.run_and_sync(&mut Box::pin(async {
            assert_eq!(Ok(()), work_scheduler.set_batch_period(5).await);
            let at_four =
                fsys::WorkRequest { start: Some(fsys::Start::MonotonicTime(4)), period: None };
            assert_eq!(Ok(()), work_scheduler.schedule_work_item(&root, "AT_FOUR", &at_four).await);
            let at_nine =
                fsys::WorkRequest { start: Some(fsys::Start::MonotonicTime(9)), period: None };
            assert_eq!(Ok(()), work_scheduler.schedule_work_item(&root, "AT_NINE", &at_nine).await);
        }));

        // Confirm timer and work items.
        t.assert_next_timer_at(5);
        t.assert_work_items(
            &work_scheduler,
            vec![
                TestWorkUnit::new(4, &root, "AT_FOUR", 4, None),
                TestWorkUnit::new(9, &root, "AT_NINE", 9, None),
            ],
        );

        // Simulate delayed dispatch: System load or some other factor caused dispatch of work to be
        // delayed beyond the deadline of _both_ units of work.
        t.set_time_and_run_timers(16);

        // Confirm timers and dispatched units.
        t.assert_no_timers();
        t.assert_no_work(&work_scheduler);
    }

    #[test]
    fn work_scheduler_time_late_timer_fire_periodic_work_item() {
        let mut t = TimeTest::new();
        let work_scheduler = t.work_scheduler();
        let root = AbsoluteMoniker::root();

        // Set period and schedule two work items, one of which _should_ be dispatched in a second
        // cycle.
        t.run_and_sync(&mut Box::pin(async {
            assert_eq!(Ok(()), work_scheduler.set_batch_period(5).await);
            let at_four =
                fsys::WorkRequest { start: Some(fsys::Start::MonotonicTime(4)), period: None };
            assert_eq!(Ok(()), work_scheduler.schedule_work_item(&root, "AT_FOUR", &at_four).await);
            let at_nine_periodic =
                fsys::WorkRequest { start: Some(fsys::Start::MonotonicTime(9)), period: Some(5) };
            assert_eq!(
                Ok(()),
                work_scheduler
                    .schedule_work_item(&root, "AT_NINE_PERIODIC_FIVE", &at_nine_periodic)
                    .await
            );
        }));

        // Confirm timer and work items.
        t.assert_next_timer_at(5);
        t.assert_work_items(
            &work_scheduler,
            vec![
                TestWorkUnit::new(4, &root, "AT_FOUR", 4, None),
                TestWorkUnit::new(9, &root, "AT_NINE_PERIODIC_FIVE", 9, Some(5)),
            ],
        );

        // Simulate _seriously_ delayed dispatch: System load or some other
        // factor caused dispatch of work to be delayed _way_ beyond the
        // deadline of _both_ units of work.
        t.set_time_and_run_timers(116);

        // Confirm timer set to next batch period, and periodic work item still queued.
        t.assert_next_timer_at(120);
        t.assert_work_items(
            &work_scheduler,
            // Time:
            //   now=116
            // WorkItem:
            //  start=9
            //  period=5
            //
            // Updated WorkItem.period should be:
            //   WorkItem.next_deadline_monotonic = 9 + 5*n
            //     where
            //       Time.now < WorkItem.next_deadline_monotonic
            //       and
            //       Time.now + WorkItem.period > WorkItem.next_deadline_monotonic
            //
            // WorkItem.next_deadline_monotonic = 119 = 9 + (22 * 5).
            vec![TestWorkUnit::new(9, &root, "AT_NINE_PERIODIC_FIVE", 119, Some(5))],
        );
    }
}

#[cfg(test)]
mod connect_tests {
    use {
        super::{WorkScheduler, WORK_SCHEDULER_CONTROL_CAPABILITY_PATH},
        crate::{
            capability::ComponentManagerCapability,
            model::{
                testing::mocks::FakeOutgoingBinder, Event, EventPayload, Hooks, Realm,
                ResolverRegistry,
            },
        },
        failure::Error,
        fidl::endpoints::ClientEnd,
        fidl_fuchsia_sys2::WorkSchedulerControlMarker,
        fuchsia_async as fasync, fuchsia_zircon as zx,
        futures::lock::Mutex,
        std::sync::Arc,
    };

    #[fasync::run_singlethreaded(test)]
    async fn connect_to_work_scheduler_control_service() -> Result<(), Error> {
        // Retain `Arc` to keep `OutgoingBinder` alive throughout test.
        let outgoing_binder = FakeOutgoingBinder::new();

        let work_scheduler = WorkScheduler::new(Arc::downgrade(&outgoing_binder)).await;
        let hooks = Hooks::new(None);
        hooks.install(WorkScheduler::hooks(&work_scheduler)).await;

        let capability_provider = Arc::new(Mutex::new(None));
        let capability = ComponentManagerCapability::LegacyService(
            WORK_SCHEDULER_CONTROL_CAPABILITY_PATH.clone(),
        );

        let (client, server) = zx::Channel::create()?;

        let realm = {
            let resolver = ResolverRegistry::new();
            let root_component_url = "test:///root".to_string();
            Arc::new(Realm::new_root_realm(resolver, root_component_url))
        };
        let event = Event {
            target_realm: realm.clone(),
            payload: EventPayload::RouteBuiltinCapability {
                capability: capability.clone(),
                capability_provider: capability_provider.clone(),
            },
        };
        hooks.dispatch(&event).await?;

        let capability_provider = capability_provider.lock().await.take();
        if let Some(capability_provider) = capability_provider {
            capability_provider.open(0, 0, String::new(), server).await?;
        }

        let work_scheduler_control = ClientEnd::<WorkSchedulerControlMarker>::new(client)
            .into_proxy()
            .expect("failed to create launcher proxy");
        let result = work_scheduler_control.get_batch_period().await;
        result
            .expect("failed to use WorkSchedulerControl service")
            .expect("WorkSchedulerControl.GetBatchPeriod() yielded error");
        Ok(())
    }
}
