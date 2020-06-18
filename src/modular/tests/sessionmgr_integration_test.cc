// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/hardware/power/statecontrol/cpp/fidl.h>
#include <fuchsia/hardware/power/statecontrol/cpp/fidl_test_base.h>
#include <fuchsia/intl/cpp/fidl.h>
#include <fuchsia/modular/internal/cpp/fidl.h>
#include <fuchsia/modular/testing/cpp/fidl.h>
#include <lib/fdio/directory.h>
#include <lib/modular/testing/cpp/fake_agent.h>
#include <lib/modular/testing/cpp/fake_component.h>
#include <lib/syslog/cpp/macros.h>

#include <gmock/gmock.h>

#include "src/lib/files/glob.h"
#include "src/lib/fsl/vmo/strings.h"
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

class MockAdmin : public fuchsia::hardware::power::statecontrol::testing::Admin_TestBase {
 public:
  bool suspend_called() { return suspend_called_; }

 private:
  void Suspend(fuchsia::hardware::power::statecontrol::SystemPowerState state,
               SuspendCallback callback) override {
    ASSERT_FALSE(suspend_called_);
    suspend_called_ = true;
    ASSERT_EQ(fuchsia::hardware::power::statecontrol::SystemPowerState::REBOOT, state);
    callback(fuchsia::hardware::power::statecontrol::Admin_Suspend_Result::WithResponse(
        fuchsia::hardware::power::statecontrol::Admin_Suspend_Response(ZX_OK)));
  }

  void Reboot(fuchsia::hardware::power::statecontrol::RebootReason reason,
              RebootCallback callback) override {
    ASSERT_FALSE(suspend_called_);
    suspend_called_ = true;
    ASSERT_EQ(fuchsia::hardware::power::statecontrol::RebootReason::SESSION_FAILURE, reason);
    callback(fuchsia::hardware::power::statecontrol::Admin_Reboot_Result::WithResponse(
        fuchsia::hardware::power::statecontrol::Admin_Reboot_Response(ZX_OK)));
  }

  // |TestBase|
  void NotImplemented_(const std::string& name) override {
    FX_NOTIMPLEMENTED() << name << " is not implemented";
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
      {.url = fake_module_url, .sandbox_services = {"fuchsia.intl.PropertyProvider"}}};
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

// Launch a session shell an ensure that it receives argv configured for it in the Modular Config.
TEST_F(SessionmgrIntegrationTest, SessionShellReceivesComponentArgsFromConfig) {
  const std::string session_shell_url = "fuchsia-pkg://fuchsia.com/fake_shell/#fake_shell.cmx";

  fuchsia::modular::testing::TestHarnessSpec spec;

  fuchsia::modular::session::SessionShellMapEntry entry;
  entry.mutable_config()->mutable_app_config()->set_url(session_shell_url);
  spec.mutable_basemgr_config()->mutable_session_shell_map()->push_back(std::move(entry));

  fuchsia::modular::testing::InterceptSpec intercept_spec;
  intercept_spec.set_component_url(session_shell_url);
  spec.mutable_components_to_intercept()->push_back(std::move(intercept_spec));

  fuchsia::modular::session::AppConfig component_arg;
  component_arg.set_url(session_shell_url);
  component_arg.mutable_args()->push_back("foo");
  spec.mutable_sessionmgr_config()->mutable_component_args()->push_back(std::move(component_arg));

  bool session_shell_running = false;
  test_harness().events().OnNewComponent =
      [&](fuchsia::sys::StartupInfo startup_info,
          fidl::InterfaceHandle<fuchsia::modular::testing::InterceptedComponent> component) {
        ASSERT_EQ(startup_info.launch_info.url, session_shell_url);
        ASSERT_TRUE(!!startup_info.launch_info.arguments);
        EXPECT_THAT(startup_info.launch_info.arguments.value(), ::testing::ElementsAre("foo"));
        session_shell_running = true;
      };

  test_harness()->Run(std::move(spec));
  RunLoopUntil([&] { return session_shell_running; });
}

TEST_F(SessionmgrIntegrationTest, RebootCalledIfSessionmgrCrashNumberReachesRetryLimit) {
  MockAdmin mock_admin;
  fidl::BindingSet<fuchsia::hardware::power::statecontrol::Admin> admin_bindings;

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
  fidl::BindingSet<fuchsia::hardware::power::statecontrol::Admin> admin_bindings;

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

  // Restart the session 4 times and show that device suspend is NOT invoked.
  for (int i = 0; i < 4; i++) {
    bool session_restarted = false;
    basemgr->RestartSession([&] { session_restarted = true; });
    RunLoopUntil([&] { return !session_shell->is_running(); });
    RunLoopUntil([&] { return session_restarted; });
    ASSERT_FALSE(mock_admin.suspend_called()) << "Suspend called on iteration #" << i;
    RunLoopUntil([&] { return session_shell->is_running(); });
  }
  ASSERT_FALSE(mock_admin.suspend_called());
}

TEST_F(SessionmgrIntegrationTest, RestartSessionAgentOnCrash) {
  std::string fake_agent_url =
      modular_testing::TestHarnessBuilder::GenerateFakeUrl("test_agent_to_restart");

  int launch_count = 0;

  fuchsia::modular::testing::TestHarnessSpec spec;
  spec.mutable_sessionmgr_config()->set_session_agents({fake_agent_url});
  modular_testing::TestHarnessBuilder builder(std::move(spec));

  std::unique_ptr<modular_testing::FakeAgent> fake_agent;
  builder.InterceptComponent({
      .url = fake_agent_url,
      .sandbox_services =
          {
              fuchsia::modular::ComponentContext::Name_,
              fuchsia::modular::AgentContext::Name_,
          },
      .launch_handler =
          [&](fuchsia::sys::StartupInfo startup_info,
              fidl::InterfaceHandle<fuchsia::modular::testing::InterceptedComponent>
                  intercepted_component) mutable {
            launch_count++;
            fake_agent =
                std::make_unique<modular_testing::FakeAgent>(modular_testing::FakeComponent::Args{
                    .url = fake_agent_url,
                });
            fake_agent->BuildInterceptOptions().launch_handler(std::move(startup_info),
                                                               std::move(intercepted_component));
          },
  });
  builder.BuildAndRun(test_harness());

  RunLoopUntil([&] { return !!fake_agent && fake_agent->is_running(); });

  ASSERT_EQ(1, launch_count);

  fake_agent->Exit(1, fuchsia::sys::TerminationReason::UNKNOWN);
  auto old_agent = std::move(fake_agent);
  fake_agent.reset();

  RunLoopUntil([&] { return !!fake_agent && fake_agent->is_running(); });

  ASSERT_EQ(2, launch_count);
}

TEST_F(SessionmgrIntegrationTest, RestartSessionOnSessionAgentCrash) {
  static const auto kFakeAgentUrl =
      modular_testing::TestHarnessBuilder::GenerateFakeUrl("test_agent");

  // Configure sessiomgr to restart the session when the agent terminates.
  fuchsia::modular::testing::TestHarnessSpec spec;
  spec.mutable_sessionmgr_config()->set_session_agents({kFakeAgentUrl});
  spec.mutable_sessionmgr_config()->set_restart_session_on_agent_crash({kFakeAgentUrl});

  modular_testing::TestHarnessBuilder builder(std::move(spec));
  auto session_shell = modular_testing::FakeSessionShell::CreateWithDefaultOptions();
  builder.InterceptSessionShell(session_shell->BuildInterceptOptions());
  auto fake_agent =
      std::make_unique<modular_testing::FakeAgent>(modular_testing::FakeComponent::Args{
          .url = kFakeAgentUrl,
          .sandbox_services = modular_testing::FakeAgent::GetDefaultSandboxServices()});
  builder.InterceptComponent(fake_agent->BuildInterceptOptions());

  builder.BuildAndRun(test_harness());

  // Wait for the session to start.
  RunLoopUntil([&] { return session_shell->is_running() && fake_agent->is_running(); });

  // Terminate the agent.
  fake_agent->Exit(1, fuchsia::sys::TerminationReason::UNKNOWN);
  RunLoopUntil([&] { return !fake_agent->is_running(); });

  // The session and agent should have restarted.
  RunLoopUntil([&] { return !session_shell->is_running(); });
  RunLoopUntil([&] { return session_shell->is_running() && fake_agent->is_running(); });
}

}  // namespace
