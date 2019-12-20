use {
    crate::{
        model::testing::mocks::ManagedNamespace,
        model::testing::{routing_test_helpers::*, test_helpers::*},
        model::{binding::Binder, moniker::AbsoluteMoniker, testing::mocks::FakeBinder},
        work_scheduler::{
            WorkScheduler, WORKER_CAPABILITY_PATH, WORK_SCHEDULER_CAPABILITY_PATH,
            WORK_SCHEDULER_CONTROL_CAPABILITY_PATH,
        },
    },
    cm_rust::{
        self, CapabilityPath, ExposeDecl, ExposeServiceProtocolDecl, ExposeSource, ExposeTarget,
        OfferDecl, OfferServiceProtocolDecl, OfferServiceSource, OfferTarget, UseDecl,
        UseServiceProtocolDecl, UseSource,
    },
    fidl_fuchsia_io::{MODE_TYPE_SERVICE, OPEN_RIGHT_READABLE},
    fidl_fuchsia_sys2 as fsys,
    std::{convert::TryFrom, ops::Deref, path::Path, sync::Arc},
};

struct BindingWorkScheduler {
    work_scheduler: Arc<WorkScheduler>,
    // Retain `Arc` to keep `Binder` alive throughout test.
    _binder: Arc<dyn Binder>,
}

impl BindingWorkScheduler {
    async fn new() -> Self {
        let _binder = FakeBinder::new();
        let work_scheduler = WorkScheduler::new(Arc::downgrade(&_binder)).await;
        Self { work_scheduler, _binder }
    }
}

// `BindingWorkScheduler` API is `Arc<WorkScheduler>` API.
impl Deref for BindingWorkScheduler {
    type Target = Arc<WorkScheduler>;

    fn deref(&self) -> &Self::Target {
        &self.work_scheduler
    }
}

async fn new_work_scheduler() -> BindingWorkScheduler {
    BindingWorkScheduler::new().await
}

async fn call_work_scheduler_svc_from_namespace(
    namespace: &ManagedNamespace,
    should_succeed: bool,
) {
    let path = &WORK_SCHEDULER_CAPABILITY_PATH;
    let dir_proxy = capability_util::get_dir_from_namespace(namespace, &path.dirname).await;
    let node_proxy = io_util::open_node(
        &dir_proxy,
        &Path::new(&path.basename),
        OPEN_RIGHT_READABLE,
        MODE_TYPE_SERVICE,
    )
    .expect("failed to open WorkScheduler service");
    let work_scheduler_proxy = fsys::WorkSchedulerProxy::new(node_proxy.into_channel().unwrap());
    let req = fsys::WorkRequest { start: Some(fsys::Start::MonotonicTime(0)), period: None };
    let res = work_scheduler_proxy.schedule_work("hippos", req).await;

    match should_succeed {
        true => assert_eq!(res.expect("failed to use WorkScheduler service"), Ok(())),
        false => {
            let err = res.expect_err("used WorkScheduler service successfully when it should fail");
            assert!(err.is_closed(), "expected channel closed error, got: {:?}", err);
        }
    }
}

async fn check_use_work_scheduler(
    routing_test: &RoutingTest,
    moniker: AbsoluteMoniker,
    should_succeed: bool,
) {
    let component_name = routing_test.bind_instance(&moniker).await.expect("bind instance failed");
    let component_resolved_url = RoutingTest::resolved_url(&component_name);
    let namespace = routing_test.mock_runner.get_namespace(&component_resolved_url).unwrap();
    call_work_scheduler_svc_from_namespace(&namespace, should_succeed).await;
}

async fn call_work_scheduler_control_svc_from_namespace(
    namespace: &ManagedNamespace,
    path: CapabilityPath,
    should_succeed: bool,
) {
    let dir_proxy = capability_util::get_dir_from_namespace(namespace, &path.dirname).await;
    let node_proxy = io_util::open_node(
        &dir_proxy,
        &Path::new(&path.basename),
        OPEN_RIGHT_READABLE,
        MODE_TYPE_SERVICE,
    )
    .expect("failed to open WorkSchedulerControl service");
    let work_scheduler_control_proxy =
        fsys::WorkSchedulerControlProxy::new(node_proxy.into_channel().unwrap());
    let res = work_scheduler_control_proxy.get_batch_period().await;

    match should_succeed {
        true => {
            res.expect("failed to use WorkSchedulerControl service")
                .expect("WorkSchedulerControl.GetBatchPeriod() yielded error");
        }
        false => {
            let err = res
                .expect_err("used WorkSchedulerControl service successfully when it should fail");
            assert!(err.is_closed(), "expected channel closed error, got: {:?}", err);
        }
    }
}

async fn check_use_work_scheduler_control(
    routing_test: &RoutingTest,
    moniker: AbsoluteMoniker,
    path: CapabilityPath,
    should_succeed: bool,
) {
    let component_name = routing_test.bind_instance(&moniker).await.expect("bind instance failed");
    let component_resolved_url = RoutingTest::resolved_url(&component_name);
    let namespace = routing_test.mock_runner.get_namespace(&component_resolved_url).unwrap();
    call_work_scheduler_control_svc_from_namespace(&namespace, path.clone(), should_succeed).await;
}

///   a
///    \
///     b
///
/// b: uses framework service /svc/fuchsia.sys2.WorkScheduler while exposing
///    /svc/fuchsia.sys2.Worker to framework
#[fuchsia_async::run_singlethreaded(test)]
async fn use_work_scheduler_with_expose_to_framework() {
    let components = vec![
        (
            "a",
            ComponentDeclBuilder::new()
                .add_lazy_child("b")
                .offer_runner_to_children(TEST_RUNNER_NAME)
                .build(),
        ),
        (
            "b",
            ComponentDeclBuilder::new()
                .expose(ExposeDecl::ServiceProtocol(ExposeServiceProtocolDecl {
                    source: ExposeSource::Self_,
                    source_path: (*WORKER_CAPABILITY_PATH).clone(),
                    target_path: (*WORKER_CAPABILITY_PATH).clone(),
                    target: ExposeTarget::Framework,
                }))
                .use_(UseDecl::ServiceProtocol(UseServiceProtocolDecl {
                    source: UseSource::Framework,
                    source_path: (*WORK_SCHEDULER_CAPABILITY_PATH).clone(),
                    target_path: (*WORK_SCHEDULER_CAPABILITY_PATH).clone(),
                }))
                .offer_runner_to_children(TEST_RUNNER_NAME)
                .build(),
        ),
    ];
    let work_scheduler = new_work_scheduler().await;
    let test = RoutingTestBuilder::new("a", components)
        .add_hooks(WorkScheduler::hooks(&work_scheduler))
        .build()
        .await;
    check_use_work_scheduler(&test, vec!["b:0"].into(), true).await;
}

///   a
///    \
///     b
///
/// b: uses framework service /svc/fuchsia.sys2.WorkScheduler without exposing
///    /svc/fuchsia.sys2.Worker
#[fuchsia_async::run_singlethreaded(test)]
async fn use_work_scheduler_without_expose() {
    let components = vec![
        (
            "a",
            ComponentDeclBuilder::new()
                .add_lazy_child("b")
                .offer_runner_to_children(TEST_RUNNER_NAME)
                .build(),
        ),
        (
            "b",
            ComponentDeclBuilder::new()
                .use_(UseDecl::ServiceProtocol(UseServiceProtocolDecl {
                    source: UseSource::Framework,
                    source_path: (*WORK_SCHEDULER_CAPABILITY_PATH).clone(),
                    target_path: (*WORK_SCHEDULER_CAPABILITY_PATH).clone(),
                }))
                .offer_runner_to_children(TEST_RUNNER_NAME)
                .build(),
        ),
    ];
    let work_scheduler = new_work_scheduler().await;
    let test = RoutingTestBuilder::new("a", components)
        .add_hooks(WorkScheduler::hooks(&work_scheduler))
        .build()
        .await;
    check_use_work_scheduler(&test, vec!["b:0"].into(), false).await;
}

///   a
///    \
///     b
///
/// b: uses framework service /svc/fuchsia.sys2.WorkScheduler while exposing
///    /svc/fuchsia.sys2.Worker to realm (not framework)
#[fuchsia_async::run_singlethreaded(test)]
async fn use_work_scheduler_with_expose_to_realm() {
    let components = vec![
        (
            "a",
            ComponentDeclBuilder::new()
                .add_lazy_child("b")
                .offer_runner_to_children(TEST_RUNNER_NAME)
                .build(),
        ),
        (
            "b",
            ComponentDeclBuilder::new()
                .expose(ExposeDecl::ServiceProtocol(ExposeServiceProtocolDecl {
                    source: ExposeSource::Self_,
                    source_path: (*WORKER_CAPABILITY_PATH).clone(),
                    target_path: (*WORKER_CAPABILITY_PATH).clone(),
                    target: ExposeTarget::Realm,
                }))
                .use_(UseDecl::ServiceProtocol(UseServiceProtocolDecl {
                    source: UseSource::Framework,
                    source_path: (*WORK_SCHEDULER_CAPABILITY_PATH).clone(),
                    target_path: (*WORK_SCHEDULER_CAPABILITY_PATH).clone(),
                }))
                .offer_runner_to_children(TEST_RUNNER_NAME)
                .build(),
        ),
    ];
    let work_scheduler = new_work_scheduler().await;
    let test = RoutingTestBuilder::new("a", components)
        .add_hooks(WorkScheduler::hooks(&work_scheduler))
        .build()
        .await;
    check_use_work_scheduler(&test, vec!["b:0"].into(), false).await;
}

///   a
///    \
///     b
///
/// b: uses WorkSchedulerControl offered by by a
#[fuchsia_async::run_singlethreaded(test)]
async fn use_work_scheduler_control_routed() {
    let offer_use_path = CapabilityPath::try_from("/svc/WorkSchedulerControl").unwrap();
    let components = vec![
        (
            "a",
            ComponentDeclBuilder::new()
                .offer(OfferDecl::ServiceProtocol(OfferServiceProtocolDecl {
                    source: OfferServiceSource::Realm,
                    source_path: (*WORK_SCHEDULER_CONTROL_CAPABILITY_PATH).clone(),
                    target_path: offer_use_path.clone(),
                    target: OfferTarget::Child("b".to_string()),
                }))
                .add_lazy_child("b")
                .offer_runner_to_children(TEST_RUNNER_NAME)
                .build(),
        ),
        (
            "b",
            ComponentDeclBuilder::new()
                .use_(UseDecl::ServiceProtocol(UseServiceProtocolDecl {
                    source: UseSource::Realm,
                    source_path: offer_use_path.clone(),
                    target_path: offer_use_path.clone(),
                }))
                .offer_runner_to_children(TEST_RUNNER_NAME)
                .build(),
        ),
    ];
    let work_scheduler = new_work_scheduler().await;

    let test = RoutingTestBuilder::new("a", components)
        .add_hooks(WorkScheduler::hooks(&work_scheduler))
        .build()
        .await;

    check_use_work_scheduler_control(&test, vec!["b:0"].into(), offer_use_path.clone(), true).await;
}

///   a
///    \
///     b
///
/// b: uses framework service /svc/fuchsia.sys2.WorkSchedulerControl from framework (not allowed)
#[fuchsia_async::run_singlethreaded(test)]
async fn use_work_scheduler_control_fail() {
    let offer_use_path = CapabilityPath::try_from("/svc/WorkSchedulerControl").unwrap();
    let components = vec![
        (
            "a",
            ComponentDeclBuilder::new()
                .offer(OfferDecl::ServiceProtocol(OfferServiceProtocolDecl {
                    source: OfferServiceSource::Realm,
                    source_path: (*WORK_SCHEDULER_CONTROL_CAPABILITY_PATH).clone(),
                    target_path: offer_use_path.clone(),
                    target: OfferTarget::Child("b".to_string()),
                }))
                .add_lazy_child("b")
                .offer_runner_to_children(TEST_RUNNER_NAME)
                .build(),
        ),
        (
            "b",
            ComponentDeclBuilder::new()
                .use_(UseDecl::ServiceProtocol(UseServiceProtocolDecl {
                    source: UseSource::Framework,
                    source_path: offer_use_path.clone(),
                    target_path: offer_use_path.clone(),
                }))
                .offer_runner_to_children(TEST_RUNNER_NAME)
                .build(),
        ),
    ];
    let work_scheduler = new_work_scheduler().await;
    let test = RoutingTestBuilder::new("a", components)
        .add_hooks(WorkScheduler::hooks(&work_scheduler))
        .build()
        .await;

    check_use_work_scheduler_control(&test, vec!["b:0"].into(), offer_use_path.clone(), false)
        .await;
}
