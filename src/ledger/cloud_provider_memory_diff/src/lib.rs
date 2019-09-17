// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![feature(async_await)]
#![recursion_limit = "256"]

mod controller;
mod error;
mod filter;
mod serialization;
mod session;
mod state;
mod utils;

use fidl_fuchsia_ledger_cloud::CloudProviderRequestStream;
use futures::future::LocalFutureObj;
use std::cell::RefCell;
use std::rc::Rc;

pub use crate::controller::CloudControllerFactory;
use crate::session::{CloudSession, CloudSessionShared};
use crate::state::Cloud;

/// A factory for instances of the cloud provider sharing the same data storage.
pub struct CloudFactory(Rc<RefCell<Cloud>>);

impl CloudFactory {
    /// Create a factory with empty storage.
    pub fn new() -> CloudFactory {
        CloudFactory(Rc::new(RefCell::new(Cloud::new())))
    }

    /// Returns a future that handles the request stream.
    pub fn spawn(&self, stream: CloudProviderRequestStream) -> LocalFutureObj<'static, ()> {
        CloudSession::new(Rc::new(CloudSessionShared::new(Rc::clone(&self.0))), stream).run()
    }
}
