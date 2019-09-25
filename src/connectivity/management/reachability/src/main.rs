// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! The reachability monitor monitors reachability state and generates an event to signal
//! changes.

extern crate fuchsia_syslog as syslog;
#[macro_use]
extern crate log;

mod eventloop;
mod worker;

use crate::eventloop::EventLoop;

fn main() -> Result<(), failure::Error> {
    syslog::init_with_tags(&["reachability"]).expect("failed to initialize logger");
    // TODO(dpradilla): use a `StructOpt` to pass in a log level option where the user can control
    // how verbose logs should be.
    // Severity is set to debug during development.
    syslog::set_severity(-2);

    info!("Starting reachability monitor!");
    let mut executor = fuchsia_async::Executor::new()?;

    let eventloop = EventLoop::new();
    executor.run_singlethreaded(eventloop.run())
}
