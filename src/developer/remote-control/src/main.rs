// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{Context as _, Error},
    fidl::endpoints::{ClientEnd, RequestStream, ServiceMarker},
    fidl_fuchsia_developer_remotecontrol as rcs,
    fidl_fuchsia_overnet::{ServiceProviderRequest, ServiceProviderRequestStream},
    futures::prelude::*,
    remote_control::RemoteControlService,
    std::sync::Arc,
};

async fn exec_server() -> Result<(), Error> {
    let (s, p) = fidl::Channel::create().context("creating ServiceProvider zx channel")?;
    let chan =
        fidl::AsyncChannel::from_channel(s).context("creating ServiceProvider async channel")?;
    let mut stream = ServiceProviderRequestStream::from_channel(chan);
    hoist::publish_service(rcs::RemoteControlMarker::NAME, ClientEnd::new(p))?;

    log::info!("published remote control service to overnet");
    let service = Arc::new(RemoteControlService::new().unwrap());

    while let Some(ServiceProviderRequest::ConnectToService {
        chan,
        info: _,
        control_handle: _control_handle,
    }) = stream.try_next().await.context("polling requests")?
    {
        let chan =
            fidl::AsyncChannel::from_channel(chan).context("failed to make async channel")?;
        let service_clone = Arc::clone(&service);

        hoist::spawn(async move {
            service_clone
                .serve_stream(rcs::RemoteControlRequestStream::from_channel(chan))
                .await
                .unwrap();
        });
    }
    Ok(())
}

fn main() -> Result<(), Error> {
    fuchsia_syslog::init_with_tags(&["remote-control"])?;

    hoist::run(async move {
        if let Err(e) = exec_server().await {
            log::error!("Error: {}", e);
            std::process::exit(1);
        }
    });
    Ok(())
}
