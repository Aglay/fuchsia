// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_LEDGER_TESTING_LOOP_CONTROLLER_TEST_LOOP_H_
#define PERIDOT_BIN_LEDGER_TESTING_LOOP_CONTROLLER_TEST_LOOP_H_

#include <memory>

#include <lib/async-testutils/test_loop.h>

#include "peridot/bin/ledger/testing/loop_controller.h"

namespace ledger {

// Implementation of a LoopController that uses a test loop. The test loop
// simulates the time in a deterministic way and does not rely on the real
// (physical) clock.
class LoopControllerTestLoop : public LoopController {
 public:
  LoopControllerTestLoop();
  ~LoopControllerTestLoop() override;

  void RunLoop() override;

  void StopLoop() override;

  std::unique_ptr<SubLoop> StartNewLoop() override;

  std::unique_ptr<CallbackWaiter> NewWaiter() override;

  async_dispatcher_t* dispatcher() override;

  fit::closure QuitLoopClosure() override;

  bool RunLoopUntil(fit::function<bool()> condition) override;

  void RunLoopFor(zx::duration duration) override;

 private:
  async::TestLoop loop_;
};

}  // namespace ledger

#endif  // PERIDOT_BIN_LEDGER_TESTING_LOOP_CONTROLLER_TEST_LOOP_H_
