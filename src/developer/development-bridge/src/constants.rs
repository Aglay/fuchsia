// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#![allow(unused)]

use std::time::Duration;

#[cfg(not(test))]
pub const SOCKET: &str = "/tmp/ascendd";

#[cfg(test)]
pub const SOCKET: &str = "/tmp/ascendd_for_testing_only";

pub const DAEMON: &str = "daemon";
pub const ASCENDD: &str = "ascendd";
// Config keys
pub const LOG_DIR: &str = "log-dir";
pub const LOG_ENABLED: &str = "log-enabled";
pub const SSH_PUB: &str = "ssh-pub";
pub const SSH_PRIV: &str = "ssh-priv";

// Environment file that keeps track of configuration files
pub const ENV_FILE: &str = ".ffx_env";

pub const MAX_RETRY_COUNT: u32 = 30;
// Number of retry attempts after which we'll try to auto-start RCS.
// Anecdotally this needs to be fairly high since ascendd can be slow to start,
// and we don't want to double-start the RCS.
pub const AUTOSTART_MIN_RETRY_COUNT: u32 = 15;
// Delay between retry attempts to find the RCS.
pub const RETRY_DELAY: Duration = Duration::from_millis(200);
