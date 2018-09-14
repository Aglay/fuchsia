// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/maxwell/config.h"

namespace maxwell {

std::ostream& operator<<(std::ostream& out, const Config& config) {
  out << "startup_agents:" << std::endl;
  for (const auto& agent : config.startup_agents) {
    out << "  " << agent << std::endl;
  }
  out << "session_agents:" << std::endl;
  for (const auto& agent : config.session_agents) {
    out << "  " << agent << std::endl;
  }
  return out;
}

}  // namespace maxwell
