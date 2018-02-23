// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lib/fsl/tasks/message_loop.h"
#include "lib/resolver/fidl/resolver.fidl.h"

#include "peridot/tests/maxwell_integration/test.h"

namespace maxwell {
namespace {

class ResolverTest : public MaxwellTestBase {
 public:
  ResolverTest()
      : resolver_(ConnectToService<resolver::Resolver>("resolver")) {}

 protected:
  resolver::ResolverPtr resolver_;
};

}  // namespace

TEST_F(ResolverTest, ResolveToModule) {
  f1dl::Array<resolver::ModuleInfoPtr> modules;
  resolver_->ResolveModules(
      "https://fuchsia-contracts.google.com/hello_contract", nullptr,
      [&](f1dl::Array<resolver::ModuleInfoPtr> modules_) {
        modules = std::move(modules_);
      });
  ASYNC_EQ(1, modules.size());
  EXPECT_EQ("https://www.example.com/hello", modules[0]->component_id);
}

// Ensure that invalid JSON does not result in a call that never completes.
TEST_F(ResolverTest, ResolveWithInvalidData) {
  bool completed = false;
  resolver_->ResolveModules(
      "foo contract", "not valid JSON",
      [&](f1dl::Array<resolver::ModuleInfoPtr> modules_) { completed = true; });
  ASYNC_CHECK(completed);
}

}  // namespace maxwell

int main(int argc, char** argv) {
  fsl::MessageLoop loop;
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
