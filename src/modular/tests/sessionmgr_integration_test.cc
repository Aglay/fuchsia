// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/device/manager/cpp/fidl.h>
#include <fuchsia/intl/cpp/fidl.h>
#include <fuchsia/modular/internal/cpp/fidl.h>
#include <fuchsia/modular/testing/cpp/fidl.h>
#include <lib/fdio/directory.h>
#include <lib/modular/testing/cpp/fake_component.h>

#include "src/lib/files/glob.h"
#include "src/lib/fsl/vmo/strings.h"
#include "src/lib/fxl/logging.h"
#include "src/modular/lib/modular_test_harness/cpp/fake_module.h"
#include "src/modular/lib/modular_test_harness/cpp/fake_session_shell.h"
#include "src/modular/lib/modular_test_harness/cpp/test_harness_fixture.h"

namespace {

constexpr char kBasemgrGlobPath[] = "/hub/r/mth_*_test/*/c/basemgr.cmx/*/out/debug/basemgr";

class SessionmgrIntegrationTest : public modular_testing::TestHarnessFixture {};

class IntlPropertyProviderImpl : public fuchsia::intl::PropertyProvider {
 public:
  int call_count() { return call_count_; }

 private:
  void GetProfile(fuchsia::intl::PropertyProvider::GetProfileCallback callback) override {
    call_count_++;
    fuchsia::intl::Profile profile;
    callback(std::move(profile));
  }

  int call_count_ = 0;
};

class MockAdmin : public fuchsia::device::manager::Administrator {
 public:
  bool suspend_called() { return suspend_called_; }

 private:
  void Suspend(uint32_t flags, SuspendCallback callback) override {
    ASSERT_FALSE(suspend_called_);
    suspend_called_ = true;
    ASSERT_EQ(fuchsia::device::manager::SUSPEND_FLAG_REBOOT, flags);
    callback(ZX_OK);
  }

  bool suspend_called_ = false;
};

// Create a service in the test harness that is also provided by the session environment. Verify
// story mods get the session's version of the service, even though the test harness's version of
// the service is still accessible outside of the story/session.
TEST_F(SessionmgrIntegrationTest, StoryModsGetServicesFromSessionEnvironment) {
  modular_testing::TestHarnessBuilder builder;
  auto session_shell = modular_testing::FakeSessionShell::CreateWithDefaultOptions();
  builder.InterceptSessionShell(session_shell->BuildInterceptOptions());

  // Add a fake fuchsia::intl::PropertyProvider to the test harness' environment.
  IntlPropertyProviderImpl fake_intl_property_provider;
  fidl::BindingSet<fuchsia::intl::PropertyProvider> intl_property_provider_bindings;
  builder.AddService(intl_property_provider_bindings.GetHandler(&fake_intl_property_provider));

  // Register a fake component to be launched as a story mod
  auto fake_module_url = modular_testing::TestHarnessBuilder::GenerateFakeUrl("fake_module");
  modular_testing::FakeModule fake_module{
      {.url = fake_module_url, .sandbox_services = {"fuchsia.intl.PropertyProvider"}},
      [](fuchsia::modular::Intent /*unused*/) {}};
  builder.InterceptComponent(fake_module.BuildInterceptOptions());

  // Create the test harness and verify the session shell is up
  builder.BuildAndRun(test_harness());
  ASSERT_FALSE(session_shell->is_running());
  RunLoopUntil([&] { return session_shell->is_running(); });

  // Add at least one module to the story. This should launch the fake_module.
  fuchsia::modular::Intent intent;
  intent.handler = fake_module_url;
  intent.action = "action";
  modular_testing::AddModToStory(test_harness(), "fake_story", "fake_modname", std::move(intent));

  ASSERT_FALSE(fake_module.is_running());
  RunLoopUntil([&] { return fake_module.is_running(); });

  // Request a fuchsia::intl::PropertyProvider from the story mod's component_context().
  // It should get the service from the session environment, not the fake
  // version registered in the test_harness, outside the session.
  // fake_intl_property_provider.call_count() should still be zero (0).
  fuchsia::intl::PropertyProviderPtr module_intl_property_provider;
  auto got_module_intl_property_provider =
      fake_module.component_context()->svc()->Connect<fuchsia::intl::PropertyProvider>(
          module_intl_property_provider.NewRequest());
  EXPECT_EQ(got_module_intl_property_provider, ZX_OK);
  bool got_profile_from_module_callback = false;
  zx_status_t get_profile_from_module_status = ZX_OK;
  module_intl_property_provider->GetProfile(
      [&](fuchsia::intl::Profile new_profile) { got_profile_from_module_callback = true; });
  module_intl_property_provider.set_error_handler(
      [&](zx_status_t status) { get_profile_from_module_status = status; });
  RunLoopUntil(
      [&] { return got_profile_from_module_callback || get_profile_from_module_status != ZX_OK; });
  ASSERT_EQ(get_profile_from_module_status, ZX_OK);
  ASSERT_EQ(fake_intl_property_provider.call_count(), 0);

  // And yet, the test_harness version of the service is still available, if requested outside of
  // the session scope. This time fake_intl_property_provider.call_count() should be one (1).
  fuchsia::intl::PropertyProviderPtr intl_property_provider;
  test_harness()->ConnectToEnvironmentService(fuchsia::intl::PropertyProvider::Name_,
                                              intl_property_provider.NewRequest().TakeChannel());

  bool got_profile_callback = false;
  zx_status_t got_profile_error = ZX_OK;
  intl_property_provider.set_error_handler([&](zx_status_t status) { got_profile_error = status; });
  intl_property_provider->GetProfile(
      [&](fuchsia::intl::Profile new_profile) { got_profile_callback = true; });
  RunLoopUntil([&] { return got_profile_callback || got_profile_error != ZX_OK; });
  ASSERT_EQ(got_profile_error, ZX_OK);
  ASSERT_EQ(fake_intl_property_provider.call_count(), 1);
}

TEST_F(SessionmgrIntegrationTest, RebootCalledIfSessionmgrCrashNumberReachesRetryLimit) {
  MockAdmin mock_admin;
  fidl::BindingSet<fuchsia::device::manager::Administrator> admin_bindings;

  auto session_shell = modular_testing::FakeSessionShell::CreateWithDefaultOptions();
  modular_testing::TestHarnessBuilder builder;
  builder.InterceptSessionShell(session_shell->BuildInterceptOptions());
  builder.AddService(admin_bindings.GetHandler(&mock_admin));
  builder.BuildAndRun(test_harness());

  // kill session_shell
  for (int i = 0; i < 4; i++) {
    RunLoopUntil([&] { return session_shell->is_running(); });
    session_shell->Exit(0);
    RunLoopUntil([&] { return !session_shell->is_running(); });
  }
  // Validate suspend is invoked

  RunLoopUntil([&] { return mock_admin.suspend_called(); });
  EXPECT_TRUE(mock_admin.suspend_called());
}

TEST_F(SessionmgrIntegrationTest, RestartSession) {
  // Setup environment with a suffix to enable globbing for basemgr's debug service
  fuchsia::modular::testing::TestHarnessSpec spec;
  spec.set_environment_suffix("test");
  modular_testing::TestHarnessBuilder builder(std::move(spec));

  // Setup a MockAdmin to check if sessionmgr restarts too many times. If the MockAdmin calls
  // suspend, then sessionmgr has reached its retry limit and we've failed to succesfully restart
  // the session.
  MockAdmin mock_admin;
  fidl::BindingSet<fuchsia::device::manager::Administrator> admin_bindings;

  // Use a session shell to determine if a session has been started.
  auto session_shell = modular_testing::FakeSessionShell::CreateWithDefaultOptions();
  builder.InterceptSessionShell(session_shell->BuildInterceptOptions());
  builder.AddService(admin_bindings.GetHandler(&mock_admin));
  builder.BuildAndRun(test_harness());
  RunLoopUntil([&] { return session_shell->is_running(); });

  // Connect to basemgr to call RestartSession
  files::Glob glob(kBasemgrGlobPath);
  ASSERT_EQ(1u, glob.size());
  const std::string path = *glob.begin();
  fuchsia::modular::internal::BasemgrDebugPtr basemgr;
  fdio_service_connect(path.c_str(), basemgr.NewRequest().TakeChannel().release());

  bool session_restarted = false;
  basemgr->RestartSession([&] { session_restarted = true; });
  RunLoopUntil([&] { return !session_shell->is_running(); });
  RunLoopUntil([&] { return session_restarted && session_shell->is_running(); });
  EXPECT_FALSE(mock_admin.suspend_called());
}

}  // namespace
