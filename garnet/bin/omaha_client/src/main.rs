// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![feature(async_await, await_macro)]

use failure::{Error, ResultExt};
use fidl_fuchsia_omaha_client::{
    OmahaClientConfigurationRequest, OmahaClientConfigurationRequestStream,
};
use fidl_fuchsia_update::{
    CheckStartedResult, ManagerRequest, ManagerRequestStream, ManagerState, State,
};
use fuchsia_component::server::ServiceFs;
use fuchsia_zircon as zx;
use futures::prelude::*;
use http_request::FuchsiaHyperHttpRequest;
use log::{error, info};
use omaha_client::{installer::stub::StubInstaller, state_machine::StateMachine};

mod configuration;
mod http_request;
mod install_plan;
mod metrics;
mod policy;
mod timer;

async fn run_fidl_server(stream: IncomingServices) -> Result<(), Error> {
    match stream {
        IncomingServices::Manager(mut stream) => {
            while let Some(request) =
                await!(stream.try_next()).context("error running Manager server")?
            {
                match request {
                    ManagerRequest::CheckNow { options, monitor, responder } => {
                        info!("Received CheckNow request with {:?} and {:?}", options, monitor);
                        responder
                            .send(CheckStartedResult::Started)
                            .context("error sending response")?;
                        if let Some(monitor) = monitor {
                            let (_stream, handle) = monitor.into_stream_and_control_handle()?;
                            handle.send_on_state(State {
                                state: Some(ManagerState::Idle),
                                version_available: None,
                            })?;
                        }
                    }
                    ManagerRequest::GetState { responder } => {
                        info!("Received GetState request");
                        responder
                            .send(State {
                                state: Some(ManagerState::Idle),
                                version_available: None,
                            })
                            .context("error sending response")?;
                    }
                    ManagerRequest::AddMonitor { monitor, control_handle: _ } => {
                        info!("Received AddMonitor request with {:?}", monitor);
                        let (_stream, handle) = monitor.into_stream_and_control_handle()?;
                        handle.send_on_state(State {
                            state: Some(ManagerState::Idle),
                            version_available: None,
                        })?;
                    }
                }
            }
        }
        IncomingServices::OmahaClientConfiguration(mut stream) => {
            while let Some(request) = await!(stream.try_next())
                .context("error running OmahaClientConfiguration server")?
            {
                match request {
                    OmahaClientConfigurationRequest::SetChannel {
                        channel,
                        allow_factory_reset,
                        responder,
                    } => {
                        info!(
                            "Received SetChannel request with {:?} and {:?}",
                            channel, allow_factory_reset
                        );
                        responder
                            .send(zx::Status::OK.into_raw())
                            .context("error sending response")?;
                    }
                    OmahaClientConfigurationRequest::GetChannel { responder } => {
                        info!("Received GetChannel request");
                        responder.send("stable-channel").context("error sending response")?;
                    }
                }
            }
        }
    }
    Ok(())
}

enum IncomingServices {
    Manager(ManagerRequestStream),
    OmahaClientConfiguration(OmahaClientConfigurationRequestStream),
}

fn main() -> Result<(), Error> {
    fuchsia_syslog::init().expect("Can't init logger");
    info!("Starting omaha client...");

    let mut executor = fuchsia_async::Executor::new().context("Error creating executor")?;

    executor.run_singlethreaded(async {
        let apps = configuration::get_apps()?;
        info!("Omaha apps: {:?}", apps);
        let config = configuration::get_config();
        info!("Update config: {:?}", config);

        let mut fs = ServiceFs::new_local();
        fs.dir("svc")
            .add_fidl_service(IncomingServices::Manager)
            .add_fidl_service(IncomingServices::OmahaClientConfiguration);
        fs.take_and_serve_directory_handle()?;
        const MAX_CONCURRENT: usize = 1000;
        let fidl_fut = fs.for_each_concurrent(MAX_CONCURRENT, |stream| {
            run_fidl_server(stream).unwrap_or_else(|e| error!("{:?}", e))
        });

        let (_metrics_reporter, cobalt_fut) = metrics::CobaltMetricsReporter::new();

        let http = FuchsiaHyperHttpRequest::new();
        let mut state_machine = StateMachine::new(
            policy::FuchsiaPolicyEngine,
            http,
            StubInstaller::default(),
            &config,
            timer::FuchsiaTimer,
        );
        await!(future::join3(fidl_fut, cobalt_fut, state_machine.start(apps).boxed()));
        Ok(())
    })
}
