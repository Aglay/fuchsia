// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{format_err, Context, Error},
    fidl_fuchsia_net_stack::StackMarker,
    fidl_fuchsia_netemul_sync::{BusMarker, BusProxy, Event, SyncManagerMarker},
    fuchsia_async::{self as fasync},
    fuchsia_component::client,
    fuchsia_syslog::fx_log_info,
    futures::TryStreamExt,
    std::io::{Read, Write},
    std::net::{SocketAddr, TcpListener, TcpStream},
    structopt::StructOpt,
};

const BUS_NAME: &str = "test-bus";
const SERVER_NAME: &str = "server";
const ROUTER_NAME: &str = "router";
const CLIENT_NAME: &str = "client";
const HELLO_MSG_REQ: &str = "Hello World from Client!";
const HELLO_MSG_RSP: &str = "Hello World from Server!";
const SERVER_IP: &str = "192.168.0.2";
const PORT: i32 = 8080;
const SERVER_DONE: i32 = 1;

pub struct BusConnection {
    bus: BusProxy,
}

impl BusConnection {
    pub fn new(client: &str) -> Result<BusConnection, Error> {
        let busm = client::connect_to_service::<SyncManagerMarker>()
            .context("SyncManager not available")?;
        let (bus, busch) = fidl::endpoints::create_proxy::<BusMarker>()?;
        busm.bus_subscribe(BUS_NAME, client, busch)?;
        Ok(BusConnection { bus })
    }

    pub async fn wait_for_client(&mut self, expect: &'static str) -> Result<(), Error> {
        let _ = self.bus.wait_for_clients(&mut vec![expect].drain(..), 0).await?;
        Ok(())
    }

    pub fn publish_code(&self, code: i32) -> Result<(), Error> {
        self.bus.publish(Event { code: Some(code), message: None, arguments: None })?;
        Ok(())
    }

    pub async fn wait_for_event(&self, code: i32) -> Result<(), Error> {
        let mut stream = self.bus.take_event_stream().try_filter_map(|event| match event {
            fidl_fuchsia_netemul_sync::BusEvent::OnBusData { data } => match data.code {
                Some(rcv_code) => {
                    if rcv_code == code {
                        futures::future::ok(Some(()))
                    } else {
                        futures::future::ok(None)
                    }
                }
                None => futures::future::ok(None),
            },
            _ => futures::future::ok(None),
        });
        stream.try_next().await?;
        Ok(())
    }
}

async fn run_router() -> Result<(), Error> {
    let stack =
        client::connect_to_service::<StackMarker>().context("failed to connect to netstack")?;
    let () = stack.enable_ip_forwarding().await.context("failed to enable ip forwarding")?;

    let bus = BusConnection::new(ROUTER_NAME)?;
    fx_log_info!("Waiting for server to finish...");
    let () = bus.wait_for_event(SERVER_DONE).await?;
    Ok(())
}

async fn run_server() -> Result<(), Error> {
    let listener =
        TcpListener::bind(&format!("{}:{}", SERVER_IP, PORT)).context("Can't bind to address")?;
    fx_log_info!("Waiting for connections...");
    let bus = BusConnection::new(SERVER_NAME)?;

    let (mut stream, remote) = listener.accept().context("Accept failed")?;
    fx_log_info!("Accepted connection from {}", remote);
    let mut buffer = [0; 512];
    let rd = stream.read(&mut buffer).context("read failed")?;

    let req = String::from_utf8(buffer[0..rd].to_vec()).context("not a valid utf8")?;
    if req != HELLO_MSG_REQ {
        return Err(format_err!("Got unexpected request from client: {}", req));
    }
    fx_log_info!("Got request {}", req);
    stream.write(HELLO_MSG_RSP.as_bytes()).context("write failed")?;
    stream.flush().context("flush failed")?;

    let () = bus.publish_code(SERVER_DONE)?;
    Ok(())
}

async fn run_client() -> Result<(), Error> {
    let mut bus = BusConnection::new(CLIENT_NAME)?;
    fx_log_info!("Waiting for router to start...");
    let () = bus.wait_for_client(ROUTER_NAME).await?;
    fx_log_info!("Waiting for server to start...");
    let () = bus.wait_for_client(SERVER_NAME).await?;

    fx_log_info!("Connecting to server...");
    let addr: SocketAddr = format!("{}:{}", SERVER_IP, PORT).parse()?;
    let mut stream = TcpStream::connect(&addr).context("Tcp connection failed")?;
    let request = HELLO_MSG_REQ.as_bytes();
    stream.write(request)?;
    stream.flush()?;

    let mut buffer = [0; 512];
    let rd = stream.read(&mut buffer)?;
    let rsp = String::from_utf8(buffer[0..rd].to_vec()).context("not a valid utf8")?;
    fx_log_info!("Got response {}", rsp);
    if rsp != HELLO_MSG_RSP {
        return Err(format_err!("Got unexpected echo from server: {}", rsp));
    }
    Ok(())
}

#[derive(StructOpt, Debug)]
struct Opt {
    #[structopt(short = "c")]
    is_child: bool,
    #[structopt(short = "r")]
    is_router: bool,
}

fn main() -> Result<(), Error> {
    fuchsia_syslog::init_with_tags(&["ip_forward"])?;
    fx_log_info!("Started");

    let opt = Opt::from_args();
    let mut executor = fasync::Executor::new().context("Error creating executor")?;
    executor.run_singlethreaded(async {
        if opt.is_child {
            run_client().await
        } else if opt.is_router {
            run_router().await
        } else {
            run_server().await
        }
    })
}
