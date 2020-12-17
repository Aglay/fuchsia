// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/// Serve 64 schemas at a time.
/// We limit to 64 because each schema is sent over a VMO and we can only have
/// 64 handles sent over a message.
// TODO(fxbug.dev/4601): Greedily fill the vmos with object delimited json, rather than
// giving every schema its own vmo.
pub const IN_MEMORY_SNAPSHOT_LIMIT: usize = 64;

// Number of seconds to wait for a single component to have its diagnostics data "pumped".
// This involves diagnostics directory traversal, contents extraction, and snapshotting.
pub const PER_COMPONENT_ASYNC_TIMEOUT_SECONDS: i64 = 10;

/// Name used by clients to connect to the feedback diagnostics protocol.
/// This protocol applies static selectors configured under config/data/feedback to
/// inspect exfiltration.
pub const FEEDBACK_ARCHIVE_ACCESSOR_NAME: &str = "fuchsia.diagnostics.FeedbackArchiveAccessor";

/// Name used by clients to connect to the legacy metrics diagnostics protocol.
/// This protocol applies static selectors configured under
/// config/data/legacy_metrics to inspect exfiltration.
pub const LEGACY_METRICS_ARCHIVE_ACCESSOR_NAME: &str =
    "fuchsia.diagnostics.LegacyMetricsArchiveAccessor";

/// The maximum number of Inspect files that can be simultaneously snapshotted and formatted per
/// reader.
pub const MAXIMUM_SIMULTANEOUS_SNAPSHOTS_PER_READER: usize = 4;

/// The maximum number of bytes in a formatted content VMO.
pub const FORMATTED_CONTENT_CHUNK_SIZE_TARGET: usize = 1 << 20; // 1 MiB

/// Store 4 MB of log messages and delete on FIFO basis.
pub const MAXIMUM_CACHED_LOGS_BYTES: usize = 4 * 1024 * 1024;
