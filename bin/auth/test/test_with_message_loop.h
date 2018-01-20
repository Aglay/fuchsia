// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This file has been copied from //peridot/bin/ledger/test as garnet cannot
// depend on upstream files. This will be replaced with the new test
// infrastructure in the near future.

#ifndef GARNET_BIN_AUTH_TEST_TEST_WITH_MESSAGE_LOOP_H_
#define GARNET_BIN_AUTH_TEST_TEST_WITH_MESSAGE_LOOP_H_

#include <functional>

#include "gtest/gtest.h"
#include "lib/fsl/tasks/message_loop.h"
#include "lib/fxl/macros.h"
#include "lib/fxl/time/time_delta.h"

namespace auth {

bool RunGivenLoopWithTimeout(
    fsl::MessageLoop* message_loop,
    fxl::TimeDelta timeout = fxl::TimeDelta::FromSeconds(1));

bool RunGivenLoopUntil(
    fsl::MessageLoop* message_loop, std::function<bool()> condition,
    fxl::TimeDelta timeout = fxl::TimeDelta::FromSeconds(1),
    fxl::TimeDelta step = fxl::TimeDelta::FromMilliseconds(10));

class TestWithMessageLoop : public ::testing::Test {
 public:
  TestWithMessageLoop() {}

 protected:
  // Runs the loop for at most |timeout|. Returns |true| if the timeout has been
  // reached.
  bool RunLoopWithTimeout(
      fxl::TimeDelta timeout = fxl::TimeDelta::FromSeconds(1));

  // Runs the loop until the condition returns true or the timeout is reached.
  // Returns |true| if the condition was met, and |false| if the timeout was
  // reached. The condition is checked at most every |step|.
  bool RunLoopUntil(std::function<bool()> condition,
                    fxl::TimeDelta timeout = fxl::TimeDelta::FromSeconds(1),
                    fxl::TimeDelta step = fxl::TimeDelta::FromMilliseconds(10));

  // Creates a closure that quits the test message loop when executed.
  fxl::Closure MakeQuitTask();

  // Creates a closure that quits the test message loop on the first time it's
  // executed. If executed a second time, it does nothing.
  fxl::Closure MakeQuitTaskOnce();

  fsl::MessageLoop message_loop_;

 private:
  FXL_DISALLOW_COPY_AND_ASSIGN(TestWithMessageLoop);
};

}  // namespace auth

#endif  // GARNET_BIN_AUTH_TEST_TEST_WITH_MESSAGE_LOOP_H_
