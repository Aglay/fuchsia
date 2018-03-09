// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/tests/maxwell_integration/test.h"

#include "lib/fsl/tasks/message_loop.h"
#include "lib/fxl/synchronization/sleep.h"

constexpr auto kYieldSleepPeriod = fxl::TimeDelta::FromMilliseconds(1);
constexpr auto kYieldBatchPeriod = fxl::TimeDelta::FromMilliseconds(0);

void Yield() {
  // Tried a combination of Thread::sleep_for (formerly required) and
  // PostDelayedTask delays for a particular test sequence:
  //
  //        PostDelayedTask
  // s        0ms  1ms
  // l   w/o: 9.8s 8s
  // e   1ns: 8s
  // e   1ms: 7.9s 7.9s
  // p  10ms: 8s
  //
  // However, we've observed some additional flakiness in the Launcher tests
  // without the sleep.
  //
  // Based on those results, opt to sleep 1ms; post delayed w/ 0ms.
  fxl::SleepFor(kYieldSleepPeriod);

  // Combinations tried:
  //                      PostQuitTask QuitNow
  //               inline    no msgs    hang (invalid call per docs)
  // SetAfterTaskCallback     hang      hang
  //      PostDelayedTask      ok        ok
  fsl::MessageLoop::GetCurrent()->task_runner()->PostDelayedTask(
      [] { fsl::MessageLoop::GetCurrent()->PostQuitTask(); },
      kYieldBatchPeriod);
  fsl::MessageLoop::GetCurrent()->Run();
}

Predicate operator&&(const Predicate& a, const Predicate& b) {
  return [&a, &b] { return a() && b(); };
}

Predicate operator||(const Predicate& a, const Predicate& b) {
  return [&a, &b] { return a() || b(); };
}

Predicate operator!(const Predicate& a) {
  return [&a] { return !a(); };
}

Predicate Deadline(const fxl::TimeDelta& duration) {
  const auto deadline = fxl::TimePoint::Now() + duration;
  return [deadline] { return fxl::TimePoint::Now() >= deadline; };
}

void Sleep(const fxl::TimeDelta& duration) {
  WaitUntil(Deadline(duration));
}

void Sleep() {
  Sleep(fxl::TimeDelta::FromMilliseconds(1500));
}

namespace maxwell {

MaxwellTestBase::MaxwellTestBase() {
  startup_context_ = app::ApplicationContext::CreateFromStartupInfo();
  auto root_environment = startup_context_->environment().get();
  FXL_CHECK(root_environment != nullptr);

  agent_launcher_ =
      std::make_unique<maxwell::AgentLauncher>(root_environment);

  child_app_services_.AddService<modular::ComponentContext>(
      [this](f1dl::InterfaceRequest<modular::ComponentContext> request) {
        child_component_context_.Connect(std::move(request));
      });
}

app::Services MaxwellTestBase::StartServices(const std::string& url) {
  app::Services services;
  auto launch_info = app::ApplicationLaunchInfo::New();
  launch_info->url = url;
  launch_info->directory_request = services.NewRequest();

  auto service_list = app::ServiceList::New();
  service_list->names.push_back(modular::ComponentContext::Name_);
  child_app_services_.AddBinding(service_list->provider.NewRequest());
  launch_info->additional_services = std::move(service_list);

  startup_context_->launcher()->CreateApplication(
      std::move(launch_info), nullptr);
  return services;
}

app::ApplicationEnvironment* MaxwellTestBase::root_environment() {
  return startup_context_->environment().get();
}

}  // namespace maxwell
