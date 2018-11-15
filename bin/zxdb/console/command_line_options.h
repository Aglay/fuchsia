// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "garnet/bin/zxdb/common/err.h"

namespace zxdb {

struct CommandLineOptions {
  std::optional<std::string> connect;
  bool debug_info = false;
  std::optional<std::string> run;
  std::optional<std::string> script_file;
  std::vector<std::string> symbol_paths;
};

// Parses the given command line into options and params.
//
// Returns an error if the command-line is badly formed. In addition, --help
// text will be returned as an error.
Err ParseCommandLine(int argc, const char* argv[], CommandLineOptions* options,
                     std::vector<std::string>* params);

}  // namespace zxdb
