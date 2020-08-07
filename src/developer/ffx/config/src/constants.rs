// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
use std::time::Duration;

pub const HOME: &str = "HOME";

// Environment file that keeps track of configuration files
pub const ENV_FILE: &str = ".ffx_env";

// Timeout for the config cache.
pub const CONFIG_CACHE_TIMEOUT: Duration = Duration::from_secs(3);
