// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use fuchsia_syslog::fx_log_info;
use std::fmt::Debug;

#[macro_export]
macro_rules! trylog {
    ($elem:expr) => {
        match $elem {
            Ok(v) => v,
            Err(e) => {
                fx_log_info!("{:?}", e);
                return;
            }
        }
    };
}

#[macro_export]
macro_rules! trylogbreak {
    ($elem:expr) => {
        match $elem {
            Ok(v) => v,
            Err(e) => {
                fx_log_info!("{:?}", e);
                break;
            }
        }
    };
}

pub fn log_error_discard_result<T, E: Debug>(r: Result<T, E>) {
    r.map(|_| ()).unwrap_or_else(|e| fx_log_info!("{:?}", e))
}
