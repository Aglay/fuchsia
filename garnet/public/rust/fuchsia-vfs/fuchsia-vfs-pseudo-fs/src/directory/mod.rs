// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Module hodling different kinds of pseudo directories and thier buidling blocks.

#[cfg(test)]
#[macro_use]
mod test_utils;

use libc::S_IRUSR;

pub mod controllable;
pub mod controlled;
pub mod entry;
pub mod simple;

/// POSIX emulation layer access attributes set by default for directories created with empty().
pub const DEFAULT_DIRECTORY_PROTECTION_ATTRIBUTES: u32 = S_IRUSR;

mod watcher_connection;
