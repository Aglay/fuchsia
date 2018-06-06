// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gtest/gtest.h"
#include "lib/gtest/test_with_loop.h"
#include "peridot/bin/user_runner/puppet_master/command_runners/update_mod_command_runner.h"

namespace fuchsia {
namespace modular {
namespace {

class UpdateModCommandRunnerTest : public gtest::TestWithLoop {
 protected:
  std::unique_ptr<UpdateModCommandRunner> runner_;
};

TEST_F(UpdateModCommandRunnerTest, EmptyTest) {
}

}  // namespace
}  // namespace modular
}  // namespace fuchsia
