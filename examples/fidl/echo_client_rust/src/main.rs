// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![deny(warnings)]

extern crate failure;
extern crate fidl;
extern crate fuchsia_app;
extern crate tokio_core;

// This is the generated crate containing FIDL bindings for the Echo service.
extern crate garnet_examples_fidl_services;
use garnet_examples_fidl_services::Echo;

// The `failure` crate provides an error trait called `Fail`,
// as well as a standard dynamically-dispatched `Error` type.
// The `ResultExt` trait adds the `context` method to errors, which allows
// us to add additional information to errors as they bubble up the stack.
use failure::{Error, ResultExt};

// The fuchsia_app crate allows us to start up applications.
use fuchsia_app::client::{ApplicationContext, Launcher};

// The Tokio Reactor is used to handle and distribute IO events.
use tokio_core::reactor;

fn main() {
    if let Err(e) = main_res() {
        println!("Error: {:?}", e);
    }
}

/// Starts an echo server, sends a message, and prints the response.
fn main_res() -> Result<(), Error> {
    let mut core = reactor::Core::new().context("Error creating core")?;
    let handle = core.handle();

    let app_context = ApplicationContext::new(&handle)
                        .context("Error creating application context")?;
    let launcher = Launcher::new(&app_context, &handle)
                        .context("Error creating application launcher")?;

    // Launch the server and connect to the echo service.
    let echo_url = String::from("echo_server_rust");
    let app = launcher.launch(echo_url, None, &handle)
                .context("Error launching echo server application")?;

    let echo = app.connect_to_service::<Echo::Service>(&handle)
                .context("Error connecting to echo server application")?;

    // Send "echo msg" to the server.
    // `response_fut` is a `Future` which returns `Option<String>`.
    let response_fut = echo.echo_string(Some(String::from("echo msg")));

    // Run `response_fut` to completion.
    let response = core.run(response_fut).context("Error getting echo response")?;
    println!("{:?}", response);

    Ok(())
}
