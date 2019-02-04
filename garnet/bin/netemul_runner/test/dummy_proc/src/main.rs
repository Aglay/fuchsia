// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![feature(async_await, await_macro, futures_api)]

use {
    failure::{format_err, Error, ResultExt},
    fidl_fuchsia_netemul_bus::{BusManagerMarker, BusMarker, BusProxy, Event},
    fuchsia_app::client,
    fuchsia_async as fasync,
    futures::TryStreamExt,
    structopt::StructOpt,
};

#[derive(StructOpt, Debug)]
struct Opt {
    #[structopt(short = "f")]
    fail: bool,
    #[structopt(short = "n", default_value = "root")]
    name: String,
    #[structopt(short = "w")]
    wait: Option<u64>,
    #[structopt(short = "p")]
    publish: Option<i32>,
    #[structopt(short = "e")]
    event: Option<i32>,
}

const BUS_NAME: &str = "test-bus";

pub struct BusConnection {
    bus: BusProxy,
}

impl BusConnection {
    pub fn new(client: &str) -> Result<BusConnection, Error> {
        let busm =
            client::connect_to_service::<BusManagerMarker>().context("BusManager not available")?;
        let (bus, busch) = fidl::endpoints::create_proxy::<BusMarker>()?;
        busm.subscribe(BUS_NAME, client, busch)?;
        Ok(BusConnection { bus })
    }

    pub async fn publish_code(&self, code: i32) -> Result<(), Error> {
        let () = await!(self.bus.ensure_publish(&mut Event {
            code: code,
            message: None,
            arguments: None,
        }))?;
        Ok(())
    }

    pub async fn get_clients(&self) -> Result<Vec<String>, Error> {
        let res = await!(self.bus.get_clients())?;
        Ok(res)
    }

    pub async fn wait_for_event(&self, code: i32) -> Result<(), Error> {
        let mut stream = self
            .bus
            .take_event_stream()
            .try_filter_map(|event| match event {
                fidl_fuchsia_netemul_bus::BusEvent::OnBusData { data } => {
                    if data.code == code {
                        futures::future::ok(Some(()))
                    } else {
                        futures::future::ok(None)
                    }
                }
                _ => futures::future::ok(None),
            });
        await!(stream.try_next())?;
        Ok(())
    }
}

async fn perform_bus_ops(
    publish: Option<i32>, wait: Option<i32>, name: String,
) -> Result<(), Error> {
    let bus = BusConnection::new(&name)?;
    if let Some(code) = wait {
        let () = await!(bus.wait_for_event(code))?;
    }
    if let Some(code) = publish {
        let () = await!(bus.publish_code(code))?;
    }
    Ok(())
}

fn main() -> Result<(), Error> {
    let opt = Opt::from_args();

    if let Some(wait) = opt.wait {
        std::thread::sleep(std::time::Duration::from_millis(wait));
    }

    if opt.publish != None || opt.event != None {
        let mut executor = fasync::Executor::new().context("Error creating executor")?;
        let () = executor.run_singlethreaded(perform_bus_ops(opt.publish, opt.event, opt.name))?;
    }

    if opt.fail {
        Err(format_err!("Failing because was asked to."))
    } else {
        Ok(())
    }
}
