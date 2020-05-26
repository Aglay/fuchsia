// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Main Overnet functionality.

#![deny(missing_docs)]

mod async_quic;
mod coding;
mod diagnostics_service;
mod fidl_tests;
mod framed_stream;
mod future_help;
mod handle_info;
mod labels;
mod link;
mod link_status_updater;
mod peer;
mod ping_tracker;
mod proxy;
mod proxy_stream;
mod proxyable_handle;
mod quic_link;
mod route_planner;
mod router;
mod routing_label;
mod runtime;
mod service_map;
mod socket_link;
mod stat_counter;
mod stream_framer;

// Export selected types from modules.
pub use future_help::log_errors;
pub use labels::{Endpoint, NodeId, NodeLinkId};
pub use link::Link;
pub use quic_link::Link as QuicLink;
pub use router::{generate_node_id, Router, RouterOptions};
pub use runtime::{run, spawn, wait_until};
pub use stream_framer::*;
