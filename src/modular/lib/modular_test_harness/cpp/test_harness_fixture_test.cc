// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/modular/lib/modular_test_harness/cpp/test_harness_fixture.h"

#include <fuchsia/modular/testing/cpp/fidl.h>
#include <lib/sys/cpp/service_directory.h>
#include <lib/sys/cpp/testing/test_with_environment.h>

#include <src/lib/files/glob.h>

#include "gmock/gmock.h"
#include "src/lib/fsl/vmo/strings.h"
#include "src/modular/lib/modular_test_harness/cpp/fake_component.h"
#include "src/modular/lib/modular_test_harness/cpp/fake_module.h"

class TestHarnessFixtureTest : public modular_testing::TestHarnessFixture {};

// Test that the TestHarnessFixture is able to launch the modular runtime by
// asserting that we can intercept a base shell.
TEST_F(TestHarnessFixtureTest, CanLaunchModular) {
  constexpr char kFakeBaseShellUrl[] =
      "fuchsia-pkg://example.com/FAKE_BASE_SHELL_PKG/fake_base_shell.cmx";

  // Setup base shell interception.
  modular_testing::TestHarnessBuilder builder;

  bool intercepted = false;
  builder.InterceptBaseShell(
      {.url = kFakeBaseShellUrl,
       .launch_handler =
           [&](fuchsia::sys::StartupInfo startup_info,
               fidl::InterfaceHandle<fuchsia::modular::testing::InterceptedComponent> component) {
             ASSERT_EQ(kFakeBaseShellUrl, startup_info.launch_info.url);
             intercepted = true;
           }});
  builder.BuildAndRun(test_harness());

  RunLoopUntil([&] { return intercepted; });
}

class TestComponent : public modular_testing::FakeComponent {
 public:
  TestComponent(fit::function<void()> on_created, fit::function<void()> on_destroyed)
      : FakeComponent({.url = modular_testing::TestHarnessBuilder::GenerateFakeUrl(),
                       .sandbox_services = {"fuchsia.modular.SessionShellContext"}}),
        on_created_(std::move(on_created)),
        on_destroyed_(std::move(on_destroyed)) {}

 protected:
  void OnCreate(fuchsia::sys::StartupInfo startup_info) override { on_created_(); }

  void OnDestroy() override { on_destroyed_(); }

  fit::function<void()> on_created_;
  fit::function<void()> on_destroyed_;
};

// Tests that FakeComponent receives lifecycle events when it is killed
// by its parent.
TEST_F(TestHarnessFixtureTest, FakeComponentLifecycle_KilledByParent) {
  modular_testing::TestHarnessBuilder builder;

  bool running = false;
  TestComponent session_shell([&] { running = true; }, [&] { running = false; });
  builder.InterceptSessionShell(session_shell.BuildInterceptOptions());
  builder.BuildAndRun(test_harness());

  RunLoopUntil([&] { return session_shell.is_running(); });
  EXPECT_TRUE(running);

  fuchsia::modular::SessionShellContextPtr session_shell_context;
  session_shell.component_context()->svc()->Connect(session_shell_context.NewRequest());
  session_shell_context->Logout();

  RunLoopUntil([&] { return !session_shell.is_running(); });
  EXPECT_FALSE(running);
}

// Tests that FakeComponent receives lifecycle events when it kills
// itself.
TEST_F(TestHarnessFixtureTest, FakeComponentLifecycle_KilledBySelf) {
  modular_testing::TestHarnessBuilder builder;

  bool running = false;
  TestComponent base_shell([&] { running = true; }, [&] { running = false; });
  builder.InterceptBaseShell(base_shell.BuildInterceptOptions());
  builder.BuildAndRun(test_harness());

  RunLoopUntil([&] { return base_shell.is_running(); });
  EXPECT_TRUE(running);

  base_shell.Exit(0);
  RunLoopUntil([&] { return !base_shell.is_running(); });
  EXPECT_FALSE(running);
}

// Tests that FakeComponent receives lifecycle events when it is killed
// using fuchsia.modular.Lifecycle that is published in its outgoing directory.
TEST_F(TestHarnessFixtureTest, FakeComponentLifecycle_KilledByLifecycleService) {
  modular_testing::TestHarnessBuilder builder;

  bool running = false;
  TestComponent base_shell([&] { running = true; }, [&] { running = false; });
  builder.InterceptBaseShell(base_shell.BuildInterceptOptions());
  builder.BuildAndRun(test_harness());

  RunLoopUntil([&] { return base_shell.is_running(); });
  EXPECT_TRUE(running);

  // Serve the outgoing() directory from FakeComponent.
  zx::channel svc_request, svc_dir;
  ASSERT_EQ(ZX_OK, zx::channel::create(0, &svc_request, &svc_dir));
  base_shell.component_context()->outgoing()->Serve(std::move(svc_request));
  sys::ServiceDirectory svc(std::move(svc_dir));

  fuchsia::modular::LifecyclePtr lifecycle;
  ASSERT_EQ(ZX_OK, svc.Connect(lifecycle.NewRequest(), "svc/fuchsia.modular.Lifecycle"));
  lifecycle->Terminate();
  RunLoopUntil([&] { return !base_shell.is_running(); });
  EXPECT_FALSE(running);
}

TEST_F(TestHarnessFixtureTest, AddModToStory) {
  modular_testing::TestHarnessBuilder builder;

  modular_testing::FakeModule mod({.url = modular_testing::TestHarnessBuilder::GenerateFakeUrl()},
                                  [](fuchsia::modular::Intent) {});
  builder.InterceptComponent(mod.BuildInterceptOptions());
  builder.BuildAndRun(test_harness());

  modular_testing::AddModToStory(test_harness(), "mystory", "mymod",
                                 fuchsia::modular::Intent{.handler = mod.url()});

  RunLoopUntil([&] { return mod.is_running(); });
}

class TestFixtureForTestingCleanup : public modular_testing::TestHarnessFixture {
 public:
  // Runs the test harness and calls |on_running| once the base shell starts
  // running.
  void RunUntilBaseShell(fit::function<void()> on_running) {
    modular_testing::TestHarnessBuilder builder;
    bool running = false;
    TestComponent base_shell([&] { running = true; }, [&] { running = false; });
    builder.InterceptBaseShell(base_shell.BuildInterceptOptions());
    builder.BuildAndRun(test_harness());

    RunLoopUntil([&] { return running; });
    on_running();
  };

  virtual ~TestFixtureForTestingCleanup() = default;

 private:
  // |TestBody()| is usually implemented by gtest's test fixture runner, but
  // since this test is exercising TestHarnessFixture directly, this method is
  // has a dummy implementation here.
  void TestBody() override {}
};

// Test that TestHarnessFixture will destroy the modular_test_harness.cmx
// component in its destructor.
TEST(TestHarnessFixtureCleanupTest, CleanupInDestructor) {
  // Test that modular_test_harness.cmx is not running.
  constexpr char kTestHarnessHubGlob[] = "/hub/c/modular_test_harness.cmx";
  bool exists = files::Glob(kTestHarnessHubGlob).size() == 1;
  EXPECT_FALSE(exists);

  // Test that TestHarnessFixture will run modular_test_harness.cmx
  {
    TestFixtureForTestingCleanup t;
    t.RunUntilBaseShell([&] {
      // check that modular_test_harness.cmx is running.
      bool exists = files::Glob(kTestHarnessHubGlob).size() == 1;
      EXPECT_TRUE(exists);
    });
  }
  // Test that the modular_test_harness.cmx is no longer running after
  // TestHarnessFixture is destroyed.
  exists = files::Glob(kTestHarnessHubGlob).size() == 1;
  EXPECT_FALSE(exists);
}
