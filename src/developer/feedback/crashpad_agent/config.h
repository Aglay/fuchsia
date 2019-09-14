// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_FEEDBACK_CRASHPAD_AGENT_CONFIG_H_
#define SRC_DEVELOPER_FEEDBACK_CRASHPAD_AGENT_CONFIG_H_

#include <stdint.h>
#include <zircon/types.h>

#include <memory>
#include <string>

namespace feedback {

struct CrashpadDatabaseConfig {
  // Directory path under which to store the Crashpad database.
  std::string path;

  // Maximum size (in kilobytes) that the Crashpad database should grow to, excluding current
  // reports being generated.
  uint64_t max_size_in_kb;
};

struct CrashServerConfig {
  // Whether to upload the crash report to a remote crash server or leave it locally.
  bool enable_upload = false;

  // URL of the remote crash server.
  //
  // We use a std::unique_ptr to set it only when relevant, i.e. when |enable_upload| is set.
  std::unique_ptr<std::string> url;
};

// Crash reporter static configuration.
//
// It is intended to represent an immutable configuration, typically loaded from a file.
struct Config {
  CrashpadDatabaseConfig crashpad_database;

  CrashServerConfig crash_server;

  // Maximum time (in milliseconds) spent collecting feedback data to attach to crash reports.
  uint64_t feedback_data_collection_timeout_in_milliseconds;
};

// Parses the JSON config at |filepath| as |config|.
zx_status_t ParseConfig(const std::string& filepath, Config* config);

}  // namespace feedback

#endif  // SRC_DEVELOPER_FEEDBACK_CRASHPAD_AGENT_CONFIG_H_
