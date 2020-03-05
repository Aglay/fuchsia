// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/shell/josh/console/console.h"
#include "src/lib/syslog/cpp/logger.h"

int main(int argc, char* argv[]) {
  syslog::InitLogger({GetProcessName()});

  return shell::ConsoleMain(argc, const_cast<const char**>(argv));
}
