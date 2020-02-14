// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/connectivity/weave/weavestack/app.h"

#include <Weave/DeviceLayer/PlatformManager.h>

namespace weavestack {

namespace {
using nl::Weave::DeviceLayer::PlatformMgr;
}  // namespace

App::App() = default;

App::~App() {
  // PostWeaveOp([](WeaveState* st) {
  // TODO: mark `st` as shutdown, so that the main loop exits.
  // });
  weave_loop_.join();
}

class App::WeaveState {};

void App::WeaveMain() {
  auto state = std::make_unique<WeaveState>();

  PlatformMgr().InitWeaveStack();
  // TODO: while (state.KeepRunning()) { do stuff }
}

}  // namespace weavestack
