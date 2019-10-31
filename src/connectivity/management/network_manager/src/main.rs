// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! The network manager allows clients to manage router device properties.

#![deny(missing_docs)]
#![deny(unreachable_patterns)]

extern crate fuchsia_syslog as syslog;
#[macro_use]
extern crate log;

mod event;
mod event_worker;
mod eventloop;
mod fidl_worker;
mod overnet_worker;

use crate::eventloop::EventLoop;

fn main() -> Result<(), failure::Error> {
    syslog::init().expect("failed to initialize logger");
    // Severity is set to debug during development.
    fuchsia_syslog::set_severity(-2);

    info!("Starting Network Manager!");
    let mut executor = fuchsia_async::Executor::new()?;

    let eventloop = EventLoop::new()?;
    let r = executor.run_singlethreaded(eventloop.run());
    warn!("Network Manager ended: {:?}", r);
    r
}
