// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![feature(async_await)]

use {
    component_manager_lib::{
        elf_runner::{ElfRunner, ProcessLauncherConnector},
        framework::{FrameworkServicesHook, RealFrameworkServiceHost},
        model::{
            hooks::*,
            testing::test_utils::list_directory,
            AbsoluteMoniker, //self,AbsoluteMoniker
            Hub,
            Model,
            ModelConfig,
            ModelParams,
        },
        startup,
    },
    failure::{self, Error},
    fidl::endpoints::ClientEnd,
    fidl::endpoints::ServiceMarker,
    fidl_fuchsia_io::{DirectoryMarker, OPEN_RIGHT_READABLE, OPEN_RIGHT_WRITABLE},
    fidl_fuchsia_test::SuiteMarker,
    fuchsia_component::server::{ServiceFs, ServiceObj},
    fuchsia_syslog as syslog, fuchsia_zircon as zx,
    futures::prelude::*,
    log::*,
    std::{path::PathBuf, process, sync::Arc},
};

/// This is a prototype used with ftf to launch v2 tests.
/// This is a temporary workaround till we get around using actual component manager.
/// We will start test as root component and using hub v2 attach its "expose/svc" directory to this
/// managers out/svc(v1 version) directory.

#[fuchsia_async::run_singlethreaded]
async fn main() -> Result<(), Error> {
    syslog::init_with_tags(&["component_manager_for_test"])?;
    let args = match startup::Arguments::from_args() {
        Ok(args) => args,
        Err(err) => {
            error!("{}\n{}", err, startup::Arguments::usage());
            return Err(err);
        }
    };

    info!("Component manager for test is starting up...");

    let resolver_registry = startup::available_resolvers()?;
    let builtin_services = Arc::new(startup::BuiltinRootServices::new(&args)?);
    let launcher_connector = ProcessLauncherConnector::new(&args, builtin_services);

    let (client_chan, server_chan) = zx::Channel::create().unwrap();
    let hub = Arc::new(Hub::new(args.root_component_url.clone()).unwrap());
    hub.open_root(OPEN_RIGHT_READABLE | OPEN_RIGHT_WRITABLE, server_chan.into()).await?;

    // TODO(fsamuel): It would be nice to refactor some of this code into a helper
    // function to create a Model and install a bunch of default hooks.
    let startup_args = startup::Arguments {
        use_builtin_process_launcher: false,
        root_component_url: "".to_string(),
    };
    let params = ModelParams {
        root_component_url: args.root_component_url,
        root_resolver_registry: resolver_registry,
        root_default_runner: Arc::new(ElfRunner::new(launcher_connector)),
        config: ModelConfig::default(),
        builtin_services: Arc::new(startup::BuiltinRootServices::new(&startup_args).unwrap()),
    };
    let model = Arc::new(Model::new(params));
    let framework_services = Arc::new(FrameworkServicesHook::new(
        (*model).clone(),
        Arc::new(RealFrameworkServiceHost::new()),
    ));
    model.hooks.install(hub.hooks()).await;
    model.hooks.install(vec![Hook::RouteFrameworkCapability(framework_services)]).await;

    match model.look_up_and_bind_instance(AbsoluteMoniker::root()).await {
        Ok(()) => {
            // TODO: Exit the component manager when the root component's binding is lost
            // (when it terminates).
        }
        Err(error) => {
            error!("Failed to bind to root component: {:?}", error);
            process::exit(1)
        }
    }

    let hub_proxy = ClientEnd::<DirectoryMarker>::new(client_chan)
        .into_proxy()
        .expect("failed to create directory proxy");

    // make sure root component exposes test suite protocol.
    let expose_dir_proxy = io_util::open_directory(
        &hub_proxy,
        &PathBuf::from("exec/expose/svc"),
        OPEN_RIGHT_READABLE | OPEN_RIGHT_WRITABLE,
    )
    .expect("Failed to open directory");

    assert_eq!(vec![SuiteMarker::DEBUG_NAME], list_directory(&expose_dir_proxy).await);

    // bind expose/svc to out/svc of this v1 component.
    let mut fs = ServiceFs::<ServiceObj<'_, ()>>::new();
    fs.add_remote("svc", expose_dir_proxy);

    fs.take_and_serve_directory_handle()?;
    fs.collect::<()>().await;
    Ok(())
}
