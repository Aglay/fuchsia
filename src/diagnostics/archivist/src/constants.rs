// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/// Serve 64 schemas at a time.
/// We limit to 64 because each schema is sent over a VMO and we can only have
/// 64 handles sent over a message.
// TODO(4601): Greedily fill the vmos with object delimited json, rather than
// giving every schema its own vmo.
pub static IN_MEMORY_SNAPSHOT_LIMIT: usize = 64;

// Number of seconds to wait before timing out various async operations;
//    1) Reading the diagnostics directory of a component, searching for inspect files.
//    2) Getting the description of a file proxy backing the inspect data.
//    3) Reading the bytes from a File.
//    4) Loading a hierachy from the deprecated inspect fidl protocol.
//    5) Converting an unpopulated data map into a populated data map.
pub static INSPECT_ASYNC_TIMEOUT_SECONDS: i64 = 5;

// Number of seconds to wait for a single component to have its diagnostics data "pumped".
// This involves diagnostics directory traversal, contents extraction, and snapshotting.
pub static PER_COMPONENT_ASYNC_TIMEOUT_SECONDS: i64 = 10;

/// Name used by clients to connect to the feedback diagnostics protocol.
/// This protocol applies static selectors configured under config/data/feedback to
/// inspect exfiltration.
pub static FEEDBACK_ARCHIVE_ACCESSOR_NAME: &str = "fuchsia.diagnostics.FeedbackArchiveAccessor";
