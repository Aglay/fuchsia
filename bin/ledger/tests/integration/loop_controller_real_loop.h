// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_LEDGER_TESTS_INTEGRATION_LOOP_CONTROLLER_REAL_LOOP_H_
#define PERIDOT_BIN_LEDGER_TESTS_INTEGRATION_LOOP_CONTROLLER_REAL_LOOP_H_

#include <functional>

#include <lib/async-loop/cpp/loop.h>

#include "peridot/bin/ledger/fidl/include/types.h"
#include "peridot/bin/ledger/testing/loop_controller.h"

namespace ledger {

// Implementation of a SubLoop that uses a real loop.
class SubLoopRealLoop : public SubLoop {
 public:
  SubLoopRealLoop() : loop_(&kAsyncLoopConfigNoAttachToThread) {
    loop_.StartThread();
  };
  ~SubLoopRealLoop() override { loop_.Shutdown(); }
  async_dispatcher_t* dispatcher() override { return loop_.dispatcher(); }

 private:
  async::Loop loop_;
};

// Implementation of a LoopController that uses a real loop.
class LoopControllerRealLoop : public LoopController {
 public:
  LoopControllerRealLoop() : loop_(&kAsyncLoopConfigAttachToThread) {}

  void RunLoop() override {
    loop_.Run();
    loop_.ResetQuit();
  }

  void StopLoop() override { loop_.Quit(); }

  std::unique_ptr<SubLoop> StartNewLoop() override {
    return std::make_unique<SubLoopRealLoop>();
  }

  async_dispatcher_t* dispatcher() override { return loop_.dispatcher(); }

  fit::closure QuitLoopClosure() override {
    return [this] { loop_.Quit(); };
  }
  bool RunLoopUntil(fit::function<bool()> condition) override;

  bool RunLoopFor(zx::duration duration) override;

  async::Loop loop_;
};

}  // namespace ledger

#endif  // PERIDOT_BIN_LEDGER_TESTS_INTEGRATION_LOOP_CONTROLLER_REAL_LOOP_H_
