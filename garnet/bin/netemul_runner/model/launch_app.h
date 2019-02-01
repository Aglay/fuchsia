// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_BIN_NETEMUL_RUNNER_MODEL_LAUNCH_APP_H_
#define GARNET_BIN_NETEMUL_RUNNER_MODEL_LAUNCH_APP_H_

#include "lib/fxl/macros.h"
#include "lib/json/json_parser.h"

namespace netemul {
namespace config {

class LaunchApp {
 public:
  LaunchApp() = default;
  LaunchApp(LaunchApp&& other) = default;

  bool ParseFromJSON(const rapidjson::Value& value, json::JSONParser* parser);

  const std::string& GetUrlOrDefault(const std::string& def) const;

  const std::string& url() const;
  const std::vector<std::string>& arguments() const;

 private:
  std::string url_;
  std::vector<std::string> arguments_;

  FXL_DISALLOW_COPY_AND_ASSIGN(LaunchApp);
};

}  // namespace config
}  // namespace netemul
#endif  // GARNET_BIN_NETEMUL_RUNNER_MODEL_LAUNCH_APP_H_
