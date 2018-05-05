// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_MAXWELL_CONFIG_H_
#define PERIDOT_BIN_MAXWELL_CONFIG_H_

#include <list>
#include <ostream>
#include <string>

namespace maxwell {

struct Config {
  // A list of Agents to start during Maxwell initialization.
  std::list<std::string> startup_agents;

  // Kronk startup URL (omit to exclude Kronk from startup).
  std::string kronk;

  // Set to true if the MI Dashboard should be started.
  bool mi_dashboard = false;
};

std::ostream& operator<<(std::ostream& out, const Config& config);

}  // namespace maxwell

#endif  // PERIDOT_BIN_MAXWELL_CONFIG_H_
