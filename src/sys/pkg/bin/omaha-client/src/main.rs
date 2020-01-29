// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::{Context as _, Error};
use fuchsia_component::server::ServiceFs;
use futures::{lock::Mutex, prelude::*};
use http_request::FuchsiaHyperHttpRequest;
use log::{error, info};
use omaha_client::state_machine::StateMachine;
use std::cell::RefCell;
use std::rc::Rc;

mod channel;
mod cobalt;
mod configuration;
mod fidl;
mod http_request;
mod inspect;
mod install_plan;
mod installer;
mod metrics;
mod observer;
mod policy;
mod storage;
mod temp_installer;
mod timer;

use configuration::ChannelSource;

fn main() -> Result<(), Error> {
    fuchsia_syslog::init().expect("Can't init logger");
    info!("Starting omaha client...");

    let mut executor = fuchsia_async::Executor::new().context("Error creating executor")?;

    executor.run_singlethreaded(async {
        let version = configuration::get_version().context("Failed to get version")?;
        let channel_configs = channel::get_configs().ok();
        info!("Omaha channel config: {:?}", channel_configs);
        let (app_set, channel_source) =
            configuration::get_app_set(&version, &channel_configs).await;
        info!("Omaha app set: {:?}", app_set.to_vec().await);
        let config = configuration::get_config(&version);
        info!("Update config: {:?}", config);

        let (metrics_reporter, cobalt_fut) = metrics::CobaltMetricsReporter::new();

        let http = FuchsiaHyperHttpRequest::new();
        let installer = temp_installer::FuchsiaInstaller::new()?;
        let stash = storage::Stash::new("omaha-client").await;
        let stash_ref = Rc::new(Mutex::new(stash));
        let state_machine = StateMachine::new(
            policy::FuchsiaPolicyEngine,
            http,
            installer,
            &config,
            timer::FuchsiaTimer,
            metrics_reporter,
            stash_ref.clone(),
            app_set.clone(),
        )
        .await;
        let state_machine_ref = Rc::new(RefCell::new(state_machine));

        let mut fs = ServiceFs::new_local();
        fs.take_and_serve_directory_handle()?;
        let inspector = fuchsia_inspect::Inspector::new();
        inspector.serve(&mut fs)?;
        let root = inspector.root();
        let configuration_node =
            inspect::ConfigurationNode::new(root.create_child("configuration"));
        configuration_node.set(&config);
        let apps_node = inspect::AppsNode::new(root.create_child("apps"));
        apps_node.set(&app_set.to_vec().await);
        let state_node = inspect::StateNode::new(root.create_child("state"));
        let schedule_node = inspect::ScheduleNode::new(root.create_child("schedule"));
        let protocol_state_node =
            inspect::ProtocolStateNode::new(root.create_child("protocol_state"));
        let last_results_node = inspect::LastResultsNode::new(root.create_child("last_results"));
        let _channel_source_property =
            root.create_string("channel_source", format!("{:?}", channel_source));

        let notify_cobalt =
            channel_source == ChannelSource::VbMeta || channel_source == ChannelSource::SysConfig;
        if notify_cobalt {
            cobalt::notify_cobalt_current_channel(app_set.clone()).await;
        }

        let fidl = fidl::FidlServer::new(
            state_machine_ref.clone(),
            stash_ref,
            app_set,
            apps_node,
            state_node,
            channel_configs,
        );

        // `.boxed_local()` was added to workaround stack overflow when we have too many levels of
        // nested async functions. Remove them when the generator optimization lands.
        future::join4(
            fidl.start(fs, schedule_node, protocol_state_node, last_results_node, notify_cobalt)
                .boxed_local(),
            StateMachine::start(state_machine_ref).boxed_local(),
            cobalt_fut,
            check_and_set_system_health().boxed_local(),
        )
        .await;
        Ok(())
    })
}

async fn check_and_set_system_health() {
    if let Err(err) = system_health_check::check_system_health().await {
        error!("error during system health check: {}", err);
        return;
    }
    info!("Marking current slot as good...");
    system_health_check::set_active_configuration_healthy().await;
}
