// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        framework::FrameworkCapability,
        model::{error::ModelError, hooks::*, AbsoluteMoniker, Realm},
    },
    cm_rust::{CapabilityPath, ExposeDecl, ExposeTarget, FrameworkCapabilityDecl},
    failure::{format_err, Error},
    fidl::endpoints::ServerEnd,
    fidl_fuchsia_sys2 as fsys, fuchsia_async as fasync, fuchsia_zircon as zx,
    futures::{future::BoxFuture, lock::Mutex, TryStreamExt},
    lazy_static::lazy_static,
    log::warn,
    std::{cmp::Ordering, convert::TryInto, sync::Arc},
};

lazy_static! {
    pub static ref WORKER_CAPABILITY_PATH: CapabilityPath =
        "/svc/fuchsia.sys2.Worker".try_into().unwrap();
    pub static ref WORK_SCHEDULER_CAPABILITY_PATH: CapabilityPath =
        "/svc/fuchsia.sys2.WorkScheduler".try_into().unwrap();
    pub static ref WORK_SCHEDULER_CONTROL_CAPABILITY_PATH: CapabilityPath =
        "/svc/fuchsia.sys2.WorkSchedulerControl".try_into().unwrap();
    pub static ref ROOT_WORK_SCHEDULER: Arc<Mutex<WorkScheduler>> =
        Arc::new(Mutex::new(WorkScheduler::new()));
}

/// `WorkItem` is a single item in the ordered-by-deadline collection maintained by `WorkScheduler`.
#[derive(Clone, Debug, Eq)]
struct WorkItem {
    /// The `AbsoluteMoniker` of the realm/component instance that owns this `WorkItem`.
    abs_moniker: AbsoluteMoniker,
    /// Unique identifier for this unit of work **relative to others with the same `abs_moniker`**.
    id: String,
    /// Next deadline for this unit of work, in monotonic time.
    next_deadline_monotonic: i64,
    /// Period between repeating this unit of work (if any), measure in nanoseconds.
    period: Option<i64>,
}

/// WorkItem default equality: identical `abs_moniker` and `id`.
impl PartialEq for WorkItem {
    fn eq(&self, other: &Self) -> bool {
        self.id == other.id && self.abs_moniker == other.abs_moniker
    }
}

impl WorkItem {
    fn new(
        abs_moniker: &AbsoluteMoniker,
        id: &str,
        next_deadline_monotonic: i64,
        period: Option<i64>,
    ) -> Self {
        WorkItem {
            abs_moniker: abs_moniker.clone(),
            id: id.to_string(),
            next_deadline_monotonic,
            period,
        }
    }

    /// Produce a canonical `WorkItem` from its identifying information: `abs_moniker` + `id`. Note
    /// that other fields are ignored in equality testing.
    fn new_by_identity(abs_moniker: &AbsoluteMoniker, id: &str) -> Self {
        WorkItem {
            abs_moniker: abs_moniker.clone(),
            id: id.to_string(),
            next_deadline_monotonic: 0,
            period: None,
        }
    }

    /// Attempt to unpack identifying info (`abs_moniker`, `id`) + `WorkRequest` into a `WorkItem`.
    /// Errors:
    /// - INVALID_ARGUMENTS: Missing or invalid `work_request.start` value.
    fn try_new(
        abs_moniker: &AbsoluteMoniker,
        id: &str,
        work_request: &fsys::WorkRequest,
    ) -> Result<Self, fsys::Error> {
        let next_deadline_monotonic = match &work_request.start {
            None => Err(fsys::Error::InvalidArguments),
            Some(start) => match start {
                fsys::Start::MonotonicTime(monotonic_time) => Ok(monotonic_time),
                _ => Err(fsys::Error::InvalidArguments),
            },
        }?;
        Ok(WorkItem::new(abs_moniker, id, *next_deadline_monotonic, work_request.period))
    }

    fn deadline_order(left: &Self, right: &Self) -> Ordering {
        left.next_deadline_monotonic.cmp(&right.next_deadline_monotonic)
    }
}

/// State maintained by a `WorkScheduler`, kept consistent via a single `Mutex`.
struct WorkSchedulerState {
    /// Scheduled work items that have not been dispatched.
    pub work_items: Vec<WorkItem>,
    /// Period between wakeup, batch, dispatch cycles.
    pub period: i64,
}

impl WorkSchedulerState {
    pub fn new() -> Self {
        WorkSchedulerState { work_items: Vec::new(), period: std::i64::MAX }
    }
}

/// Provides a common facility for scheduling canceling work. Each component instance manages its
/// work items in isolation from each other, but the `WorkScheduler` maintains a collection of all
/// items to make global scheduling decisions.
#[derive(Clone)]
pub struct WorkScheduler {
    inner: Arc<WorkSchedulerInner>,
}

struct WorkSchedulerInner {
    state: Mutex<WorkSchedulerState>,
}

impl WorkSchedulerInner {
    pub fn new() -> Self {
        Self { state: Mutex::new(WorkSchedulerState::new()) }
    }

    async fn on_route_capability_async<'a>(
        self: Arc<Self>,
        realm: Arc<Realm>,
        capability_decl: &'a FrameworkCapabilityDecl,
        capability: Option<Box<dyn FrameworkCapability>>,
    ) -> Result<Option<Box<dyn FrameworkCapability>>, ModelError> {
        match capability_decl {
            FrameworkCapabilityDecl::LegacyService(capability_path)
                if *capability_path == *WORK_SCHEDULER_CAPABILITY_PATH =>
            {
                Self::check_for_worker(&*realm).await?;
                Ok(Some(Box::new(WorkSchedulerCapability::new(
                    realm.abs_moniker.clone(),
                    WorkScheduler { inner: self.clone() },
                )) as Box<dyn FrameworkCapability>))
            }
            _ => Ok(capability),
        }
    }

    async fn check_for_worker(realm: &Realm) -> Result<(), ModelError> {
        let realm_state = realm.lock_state().await;
        let realm_state = realm_state.as_ref().expect("check_for_worker: not resolved");
        let decl = realm_state.decl();
        decl.exposes
            .iter()
            .find(|&expose| match expose {
                ExposeDecl::LegacyService(ls) => ls.target_path == *WORKER_CAPABILITY_PATH,
                _ => false,
            })
            .map_or_else(
                || {
                    Err(ModelError::capability_discovery_error(format_err!(
                        "component uses WorkScheduler without exposing Worker: {}",
                        realm.abs_moniker
                    )))
                },
                |expose| match expose {
                    ExposeDecl::LegacyService(ls) => match ls.target {
                        ExposeTarget::Framework => Ok(()),
                        _ => Err(ModelError::capability_discovery_error(format_err!(
                            "component exposes Worker, but not as legacy service to framework: {}",
                            realm.abs_moniker
                        ))),
                    },
                    _ => Err(ModelError::capability_discovery_error(format_err!(
                        "component exposes Worker, but not as legacy service to framework: {}",
                        realm.abs_moniker
                    ))),
                },
            )
    }
}

impl WorkScheduler {
    pub fn new() -> Self {
        Self { inner: Arc::new(WorkSchedulerInner::new()) }
    }

    pub fn hooks(&self) -> Vec<HookRegistration> {
        vec![HookRegistration {
            event_type: EventType::RouteFrameworkCapability,
            callback: self.inner.clone(),
        }]
    }

    pub async fn schedule_work(
        &self,
        abs_moniker: &AbsoluteMoniker,
        work_id: &str,
        work_request: &fsys::WorkRequest,
    ) -> Result<(), fsys::Error> {
        let mut state = self.inner.state.lock().await;
        let work_items = &mut state.work_items;
        let work_item = WorkItem::try_new(abs_moniker, work_id, work_request)?;

        if work_items.contains(&work_item) {
            return Err(fsys::Error::InstanceAlreadyExists);
        }

        work_items.push(work_item);
        work_items.sort_by(WorkItem::deadline_order);
        Ok(())
    }

    pub async fn cancel_work(
        &self,
        abs_moniker: &AbsoluteMoniker,
        work_id: &str,
    ) -> Result<(), fsys::Error> {
        let mut state = self.inner.state.lock().await;
        let work_items = &mut state.work_items;
        let work_item = WorkItem::new_by_identity(abs_moniker, work_id);

        // TODO(markdittmer): Use `work_items.remove_item(work_item)` if/when it becomes stable.
        let mut found = false;
        work_items.retain(|item| {
            let matches = &work_item == item;
            found = found || matches;
            !matches
        });

        if !found {
            return Err(fsys::Error::InstanceNotFound);
        }

        Ok(())
    }

    pub async fn get_batch_period(&self) -> Result<i64, fsys::Error> {
        let state = self.inner.state.lock().await;
        Ok(state.period)
    }

    pub async fn set_batch_period(&mut self, period: i64) -> Result<(), fsys::Error> {
        let mut state = self.inner.state.lock().await;
        state.period = period;
        Ok(())
    }

    pub async fn serve_root_work_scheduler_control(
        mut stream: fsys::WorkSchedulerControlRequestStream,
    ) -> Result<(), Error> {
        while let Some(request) = stream.try_next().await? {
            match request {
                fsys::WorkSchedulerControlRequest::GetBatchPeriod { responder, .. } => {
                    let root_work_scheduler = ROOT_WORK_SCHEDULER.lock().await;
                    let mut result = root_work_scheduler.get_batch_period().await;
                    responder.send(&mut result)?;
                }
                fsys::WorkSchedulerControlRequest::SetBatchPeriod {
                    responder,
                    batch_period,
                    ..
                } => {
                    let mut root_work_scheduler = ROOT_WORK_SCHEDULER.lock().await;
                    let mut result = root_work_scheduler.set_batch_period(batch_period).await;
                    responder.send(&mut result)?;
                }
            }
        }

        Ok(())
    }
}

impl Hook for WorkSchedulerInner {
    fn on<'a>(self: Arc<Self>, event: &'a Event<'_>) -> BoxFuture<'a, Result<(), ModelError>> {
        Box::pin(async move {
            if let Event::RouteFrameworkCapability { realm, capability_decl, capability } = event {
                let mut capability = capability.lock().await;
                *capability = self
                    .on_route_capability_async(realm.clone(), capability_decl, capability.take())
                    .await?;
            }
            Ok(())
        })
    }
}

/// `Capability` to invoke `WorkScheduler` FIDL API bound to a particular `WorkScheduler` object and
/// component instance's `AbsoluteMoniker`. All FIDL operations bound to the same object and moniker
/// observe the same collection of `WorkItem` objects.
struct WorkSchedulerCapability {
    abs_moniker: AbsoluteMoniker,
    work_scheduler: WorkScheduler,
}

impl WorkSchedulerCapability {
    pub fn new(abs_moniker: AbsoluteMoniker, work_scheduler: WorkScheduler) -> Self {
        WorkSchedulerCapability { abs_moniker, work_scheduler }
    }

    /// Service `open` invocation via an event loop that dispatches FIDL operations to
    /// `work_scheduler`.
    async fn open_async(
        work_scheduler: WorkScheduler,
        abs_moniker: &AbsoluteMoniker,
        mut stream: fsys::WorkSchedulerRequestStream,
    ) -> Result<(), Error> {
        while let Some(request) = stream.try_next().await? {
            match request {
                fsys::WorkSchedulerRequest::ScheduleWork {
                    responder,
                    work_id,
                    work_request,
                    ..
                } => {
                    let mut result =
                        work_scheduler.schedule_work(abs_moniker, &work_id, &work_request).await;
                    responder.send(&mut result)?;
                }
                fsys::WorkSchedulerRequest::CancelWork { responder, work_id, .. } => {
                    let mut result = work_scheduler.cancel_work(abs_moniker, &work_id).await;
                    responder.send(&mut result)?;
                }
            }
        }
        Ok(())
    }
}

impl FrameworkCapability for WorkSchedulerCapability {
    /// Spawn an event loop to service `WorkScheduler` FIDL operations.
    fn open(
        &self,
        _flags: u32,
        _open_mode: u32,
        _relative_path: String,
        server_end: zx::Channel,
    ) -> BoxFuture<Result<(), ModelError>> {
        let server_end = ServerEnd::<fsys::WorkSchedulerMarker>::new(server_end);
        let stream: fsys::WorkSchedulerRequestStream = server_end.into_stream().unwrap();
        let work_scheduler = self.work_scheduler.clone();
        let abs_moniker = self.abs_moniker.clone();
        fasync::spawn(async move {
            let result = Self::open_async(work_scheduler, &abs_moniker, stream).await;
            if let Err(e) = result {
                // TODO(markdittmer): Set an epitaph to indicate this was an unexpected error.
                warn!("WorkSchedulerCapability.open failed: {}", e);
            }
        });

        Box::pin(async { Ok(()) })
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        crate::model::{AbsoluteMoniker, ChildMoniker},
    };

    /// Time is measured in nanoseconds. This provides a constant symbol for one second.
    const SECOND: i64 = 1000000000;

    // Use arbitrary start monolithic time. This will surface bugs that, for example, are not
    // apparent when "time starts at 0".
    const FAKE_MONOTONIC_TIME: i64 = 374789234875;

    async fn get_work_status(
        work_scheduler: &WorkScheduler,
        abs_moniker: &AbsoluteMoniker,
        work_id: &str,
    ) -> Result<(i64, Option<i64>), fsys::Error> {
        let state = work_scheduler.inner.state.lock().await;
        let work_items = &state.work_items;
        match work_items
            .iter()
            .find(|work_item| &work_item.abs_moniker == abs_moniker && work_item.id == work_id)
        {
            Some(work_item) => Ok((work_item.next_deadline_monotonic, work_item.period)),
            None => Err(fsys::Error::InstanceNotFound),
        }
    }

    async fn get_all_by_deadline(work_scheduler: &WorkScheduler) -> Vec<WorkItem> {
        let state = work_scheduler.inner.state.lock().await;
        state.work_items.clone()
    }

    fn child(parent: &AbsoluteMoniker, name: &str) -> AbsoluteMoniker {
        parent.child(ChildMoniker::new(name.to_string(), None, 0))
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn work_scheduler_basic() {
        let work_scheduler = WorkScheduler::new();
        let root = AbsoluteMoniker::root();
        let a = child(&root, "a");
        let b = child(&a, "b");
        let c = child(&b, "c");

        let now_once = fsys::WorkRequest {
            start: Some(fsys::Start::MonotonicTime(FAKE_MONOTONIC_TIME)),
            period: None,
        };
        let each_second = fsys::WorkRequest {
            start: Some(fsys::Start::MonotonicTime(FAKE_MONOTONIC_TIME + SECOND)),
            period: Some(SECOND),
        };
        let in_an_hour = fsys::WorkRequest {
            start: Some(fsys::Start::MonotonicTime(FAKE_MONOTONIC_TIME + (SECOND * 60 * 60))),
            period: None,
        };

        // Schedule different 2 out of 3 requests on each component instance.

        assert_eq!(Ok(()), work_scheduler.schedule_work(&a, "NOW_ONCE", &now_once).await);
        assert_eq!(Ok(()), work_scheduler.schedule_work(&a, "EACH_SECOND", &each_second).await);

        assert_eq!(Ok(()), work_scheduler.schedule_work(&b, "EACH_SECOND", &each_second).await);
        assert_eq!(Ok(()), work_scheduler.schedule_work(&b, "IN_AN_HOUR", &in_an_hour).await);

        assert_eq!(Ok(()), work_scheduler.schedule_work(&c, "IN_AN_HOUR", &in_an_hour).await);
        assert_eq!(Ok(()), work_scheduler.schedule_work(&c, "NOW_ONCE", &now_once).await);

        assert_eq!(
            Ok((FAKE_MONOTONIC_TIME, None)),
            get_work_status(&work_scheduler, &a, "NOW_ONCE").await
        );
        assert_eq!(
            Ok((FAKE_MONOTONIC_TIME + SECOND, Some(SECOND))),
            get_work_status(&work_scheduler, &a, "EACH_SECOND").await
        );
        assert_eq!(
            Err(fsys::Error::InstanceNotFound),
            get_work_status(&work_scheduler, &a, "IN_AN_HOUR").await
        );

        assert_eq!(
            Err(fsys::Error::InstanceNotFound),
            get_work_status(&work_scheduler, &b, "NOW_ONCE").await
        );
        assert_eq!(
            Ok((FAKE_MONOTONIC_TIME + SECOND, Some(SECOND))),
            get_work_status(&work_scheduler, &b, "EACH_SECOND").await
        );
        assert_eq!(
            Ok((FAKE_MONOTONIC_TIME + (SECOND * 60 * 60), None)),
            get_work_status(&work_scheduler, &b, "IN_AN_HOUR").await
        );

        assert_eq!(
            Ok((FAKE_MONOTONIC_TIME, None)),
            get_work_status(&work_scheduler, &c, "NOW_ONCE").await
        );
        assert_eq!(
            Err(fsys::Error::InstanceNotFound),
            get_work_status(&work_scheduler, &c, "EACH_SECOND").await
        );
        assert_eq!(
            Ok((FAKE_MONOTONIC_TIME + (SECOND * 60 * 60), None)),
            get_work_status(&work_scheduler, &c, "IN_AN_HOUR").await
        );

        // Cancel a's NOW_ONCE. Confirm it only affects a's scheduled work.

        assert_eq!(Ok(()), work_scheduler.cancel_work(&a, "NOW_ONCE").await);

        assert_eq!(
            Err(fsys::Error::InstanceNotFound),
            get_work_status(&work_scheduler, &a, "NOW_ONCE").await
        );
        assert_eq!(
            Ok((FAKE_MONOTONIC_TIME + SECOND, Some(SECOND))),
            get_work_status(&work_scheduler, &a, "EACH_SECOND").await
        );
        assert_eq!(
            Err(fsys::Error::InstanceNotFound),
            get_work_status(&work_scheduler, &a, "IN_AN_HOUR").await
        );

        assert_eq!(
            Err(fsys::Error::InstanceNotFound),
            get_work_status(&work_scheduler, &b, "NOW_ONCE").await
        );
        assert_eq!(
            Ok((FAKE_MONOTONIC_TIME + SECOND, Some(SECOND))),
            get_work_status(&work_scheduler, &b, "EACH_SECOND").await
        );
        assert_eq!(
            Ok((FAKE_MONOTONIC_TIME + (SECOND * 60 * 60), None)),
            get_work_status(&work_scheduler, &b, "IN_AN_HOUR").await
        );

        assert_eq!(
            Ok((FAKE_MONOTONIC_TIME, None)),
            get_work_status(&work_scheduler, &c, "NOW_ONCE").await
        );
        assert_eq!(
            Err(fsys::Error::InstanceNotFound),
            get_work_status(&work_scheduler, &c, "EACH_SECOND").await
        );
        assert_eq!(
            Ok((FAKE_MONOTONIC_TIME + (SECOND * 60 * 60), None)),
            get_work_status(&work_scheduler, &c, "IN_AN_HOUR").await
        );
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn work_scheduler_deadline_order() {
        let work_scheduler = WorkScheduler::new();
        let root = AbsoluteMoniker::root();
        let a = child(&root, "a");
        let b = child(&a, "b");
        let c = child(&b, "c");

        let now_once = fsys::WorkRequest {
            start: Some(fsys::Start::MonotonicTime(FAKE_MONOTONIC_TIME)),
            period: None,
        };
        let each_second = fsys::WorkRequest {
            start: Some(fsys::Start::MonotonicTime(FAKE_MONOTONIC_TIME + SECOND)),
            period: Some(SECOND),
        };
        let in_an_hour = fsys::WorkRequest {
            start: Some(fsys::Start::MonotonicTime(FAKE_MONOTONIC_TIME + (SECOND * 60 * 60))),
            period: None,
        };

        assert_eq!(Ok(()), work_scheduler.schedule_work(&a, "EACH_SECOND", &each_second).await);
        assert_eq!(Ok(()), work_scheduler.schedule_work(&c, "NOW_ONCE", &now_once).await);
        assert_eq!(Ok(()), work_scheduler.schedule_work(&b, "IN_AN_HOUR", &in_an_hour).await);

        // Order should match deadlines, not order of scheduling or component topology.
        assert_eq!(
            vec![
                WorkItem::new(&c, "NOW_ONCE", FAKE_MONOTONIC_TIME, None),
                WorkItem::new(&a, "EACH_SECOND", FAKE_MONOTONIC_TIME + SECOND, Some(SECOND),),
                WorkItem::new(&b, "IN_AN_HOUR", FAKE_MONOTONIC_TIME + (SECOND * 60 * 60), None,),
            ],
            get_all_by_deadline(&work_scheduler).await
        );
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn work_scheduler_batch_period() {
        let mut work_scheduler = WorkScheduler::new();
        assert_eq!(Ok(std::i64::MAX), work_scheduler.get_batch_period().await);
        assert_eq!(Ok(()), work_scheduler.set_batch_period(SECOND).await);
        assert_eq!(Ok(SECOND), work_scheduler.get_batch_period().await)
    }
}
