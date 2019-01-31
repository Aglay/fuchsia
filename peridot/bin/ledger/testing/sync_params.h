// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_LEDGER_TESTING_SYNC_PARAMS_H_
#define PERIDOT_BIN_LEDGER_TESTING_SYNC_PARAMS_H_

#include <set>
#include <string>

#include <lib/component/cpp/startup_context.h>
#include <lib/fxl/command_line.h>
#include <lib/fxl/strings/string_view.h>

#include "peridot/lib/firebase_auth/testing/credentials.h"

namespace ledger {

// Parameters needed to configure synchronization against a real server.
struct SyncParams {
  SyncParams();
  SyncParams(const SyncParams& other);
  SyncParams(SyncParams&& other);
  SyncParams& operator=(const SyncParams& other);
  SyncParams& operator=(SyncParams&& other);

  // API key used to access the database.
  std::string api_key;

  // Credentials for the cloud service.
  std::unique_ptr<service_account::Credentials> credentials;
};

// Returns a string listing the command-line parameters which need to be
// provided for a benchmark to connect to a cloud server.
std::string GetSyncParamsUsage();

// Reads the sync parameters from the command-line. Prints a warning and returns
// false if these parameters are missing or cannot be parsed.
bool ParseSyncParamsFromCommandLine(const fxl::CommandLine& command_line,
                                    component::StartupContext* startup_context,
                                    SyncParams* sync_params);

// Returns the names of the flags parsed from the command line by
// ParseSyncParamsFromCommandLine(), without the leading "--".
std::set<std::string> GetSyncParamFlags();

}  // namespace ledger

#endif  // PERIDOT_BIN_LEDGER_TESTING_SYNC_PARAMS_H_
