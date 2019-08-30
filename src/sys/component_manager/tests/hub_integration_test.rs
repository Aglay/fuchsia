// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![feature(async_await)]

use {
    component_manager_lib::{
        elf_runner::{ElfRunner, ProcessLauncherConnector},
        framework::RealmServiceHost,
        model::{
            self,
            hooks::*,
            testing::test_utils::{list_directory, read_file},
            Hub, Model, ModelParams,
        },
        startup,
    },
    failure::{self, Error},
    fidl::endpoints::ClientEnd,
    fidl_fidl_examples_routing_echo as fecho,
    fidl_fuchsia_io::{
        DirectoryMarker, DirectoryProxy, MODE_TYPE_SERVICE, OPEN_RIGHT_READABLE,
        OPEN_RIGHT_WRITABLE,
    },
    fuchsia_zircon as zx,
    hub_test_hook::*,
    std::{path::PathBuf, sync::Arc, vec::Vec},
};

async fn connect_to_echo_service(hub_proxy: &DirectoryProxy, echo_service_path: String) {
    let node_proxy = io_util::open_node(
        &hub_proxy,
        &PathBuf::from(echo_service_path),
        io_util::OPEN_RIGHT_READABLE,
        MODE_TYPE_SERVICE,
    )
    .expect("failed to open echo service");
    let echo_proxy = fecho::EchoProxy::new(node_proxy.into_channel().unwrap());
    let res = echo_proxy.echo_string(Some("hippos")).await;
    assert_eq!(res.expect("failed to use echo service"), Some("hippos".to_string()));
}

fn hub_directory_listing(listing: Vec<&str>) -> HubReportEvent {
    HubReportEvent::DirectoryListing(listing.iter().map(|s| s.to_string()).collect())
}

fn file_content(content: &str) -> HubReportEvent {
    HubReportEvent::FileContent(content.to_string())
}

#[fuchsia_async::run_singlethreaded(test)]
async fn test() -> Result<(), Error> {
    let args = startup::Arguments { use_builtin_process_launcher: false, ..Default::default() };
    let builtin_services = Arc::new(startup::BuiltinRootServices::new(&args)?);
    let launcher_connector = ProcessLauncherConnector::new(&args, builtin_services);
    let runner = ElfRunner::new(launcher_connector);
    let resolver_registry = startup::available_resolvers()?;
    let root_component_url =
        "fuchsia-pkg://fuchsia.com/hub_integration_test#meta/echo_realm.cm".to_string();

    let (client_chan, server_chan) = zx::Channel::create().unwrap();
    let hub = Arc::new(Hub::new(root_component_url.clone()).unwrap());
    hub.open_root(OPEN_RIGHT_READABLE | OPEN_RIGHT_WRITABLE, server_chan.into()).await?;
    let hub_test_hook = Arc::new(HubTestHook::new());

    let startup_args = startup::Arguments {
        use_builtin_process_launcher: false,
        root_component_url: "".to_string(),
    };
    let params = ModelParams {
        root_component_url: root_component_url,
        root_resolver_registry: resolver_registry,
        root_default_runner: Arc::new(runner),
        config: model::ModelConfig::default(),
        builtin_services: Arc::new(startup::BuiltinRootServices::new(&startup_args).unwrap()),
    };

    let model = Arc::new(Model::new(params));
    let realm_service_host = RealmServiceHost::new((*model).clone());
    model.hooks.install(realm_service_host.hooks()).await;
    model.hooks.install(hub.hooks()).await;
    model.hooks.install(vec![Hook::RouteFrameworkCapability(hub_test_hook.clone())]).await;

    let res = model.look_up_and_bind_instance(model::AbsoluteMoniker::root()).await;
    let expected_res: Result<(), model::ModelError> = Ok(());
    assert_eq!(format!("{:?}", res), format!("{:?}", expected_res));

    let hub_proxy = ClientEnd::<DirectoryMarker>::new(client_chan)
        .into_proxy()
        .expect("failed to create directory proxy");

    // Verify that echo_realm has two children.
    let children_dir_proxy = io_util::open_directory(
        &hub_proxy,
        &PathBuf::from("children"),
        OPEN_RIGHT_READABLE | OPEN_RIGHT_WRITABLE,
    )
    .expect("Failed to open directory");
    assert_eq!(vec!["echo_server:0", "hub_client:0"], list_directory(&children_dir_proxy).await);

    // These args are from hub_client.cml.
    assert_eq!("Hippos", read_file(&hub_proxy, "children/hub_client:0/exec/runtime/args/0").await);
    assert_eq!("rule!", read_file(&hub_proxy, "children/hub_client:0/exec/runtime/args/1").await);

    let echo_service_name = "fidl.examples.routing.echo.Echo";
    let hub_report_service_name = "fuchsia.test.hub.HubReport";
    let expose_svc_dir = "children/echo_server:0/exec/expose/svc";
    let expose_svc_dir_proxy = io_util::open_directory(
        &hub_proxy,
        &PathBuf::from(expose_svc_dir.clone()),
        OPEN_RIGHT_READABLE | OPEN_RIGHT_WRITABLE,
    )
    .expect("Failed to open directory");
    assert_eq!(vec![echo_service_name], list_directory(&expose_svc_dir_proxy).await);

    let in_dir = "children/hub_client:0/exec/in";
    let svc_dir = format!("{}/{}", in_dir, "svc");
    let svc_dir_proxy = io_util::open_directory(
        &hub_proxy,
        &PathBuf::from(svc_dir.clone()),
        OPEN_RIGHT_READABLE | OPEN_RIGHT_WRITABLE,
    )
    .expect("Failed to open directory");
    assert_eq!(
        vec![echo_service_name, hub_report_service_name],
        list_directory(&svc_dir_proxy).await
    );

    // Verify that the 'pkg' directory is avaialble.
    let pkg_dir = format!("{}/{}", in_dir, "pkg");
    let pkg_dir_proxy = io_util::open_directory(
        &hub_proxy,
        &PathBuf::from(pkg_dir),
        OPEN_RIGHT_READABLE | OPEN_RIGHT_WRITABLE,
    )
    .expect("Failed to open directory");
    assert_eq!(vec!["bin", "lib", "meta", "test"], list_directory(&pkg_dir_proxy).await);

    // Verify that we can connect to the echo service from the in/svc directory.
    let in_echo_service_path = format!("{}/{}", svc_dir, echo_service_name);
    connect_to_echo_service(&hub_proxy, in_echo_service_path).await;

    // Verify that we can connect to the echo service from the expose/svc directory.
    let expose_echo_service_path = format!("{}/{}", expose_svc_dir, echo_service_name);
    connect_to_echo_service(&hub_proxy, expose_echo_service_path).await;

    // Verify that the 'hub' directory is avaialble. The 'hub' mapped to 'hub_client''s
    // namespace is actually mapped to the 'exec' directory of 'hub_client'.
    let scoped_hub_dir = format!("{}/{}", in_dir, "hub");
    let scoped_hub_dir_proxy = io_util::open_directory(
        &hub_proxy,
        &PathBuf::from(scoped_hub_dir),
        OPEN_RIGHT_READABLE | OPEN_RIGHT_WRITABLE,
    )
    .expect("Failed to open directory");
    assert_eq!(
        vec!["expose", "in", "out", "resolved_url", "runtime"],
        list_directory(&scoped_hub_dir_proxy).await
    );

    // Verify that hub_client's view of the hub matches the view reachable from
    // the global hub.
    assert_eq!(
        hub_directory_listing(vec!["expose", "in", "out", "resolved_url", "runtime"]),
        hub_test_hook.observe("/hub").await
    );

    // Verify that hub_client's view is able to correctly read the names of the
    // children of the parent echo_realm.
    assert_eq!(
        hub_directory_listing(vec!["echo_server:0", "hub_client:0"]),
        hub_test_hook.observe("/parent_hub/children").await
    );

    // Verify that hub_client is able to see its sibling's hub correctly.
    assert_eq!(
        file_content("fuchsia-pkg://fuchsia.com/hub_integration_test#meta/echo_server.cm"),
        hub_test_hook.observe("/sibling_hub/exec/resolved_url").await
    );

    Ok(())
}
