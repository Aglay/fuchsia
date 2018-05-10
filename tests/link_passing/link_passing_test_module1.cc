// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/cpp/modular.h>
#include <fuchsia/cpp/views_v1.h>
#include "lib/app_driver/cpp/module_driver.h"
#include "lib/fsl/tasks/message_loop.h"
#include "peridot/lib/testing/reporting.h"
#include "peridot/lib/testing/testing.h"
#include "peridot/tests/common/defs.h"
#include "peridot/tests/link_passing/defs.h"

using modular::testing::TestPoint;

namespace {

// Cf. README.md for what this test does and how.
class TestApp : modular::LinkWatcher {
 public:
  TestApp(
      modular::ModuleHost* const module_host,
      fidl::InterfaceRequest<views_v1::ViewProvider> /*view_provider_request*/,
      fidl::InterfaceRequest<
      component::ServiceProvider> /*outgoing_services*/)
      : module_host_(module_host),
        link1_watcher_binding_(this),
        link2_watcher_binding_(this) {
    modular::testing::Init(module_host->application_context(), __FILE__);
    modular::testing::GetStore()->Put("module1_init", "", [] {});
    module_host_->module_context()->Ready();

    Start();
  }

  void Start() {
    module_host_->module_context()->GetLink("link", link1_.NewRequest());
    link1_->WatchAll(link1_watcher_binding_.NewBinding());

    module_host_->module_context()->GetLink(nullptr, link2_.NewRequest());
    link2_->WatchAll(link2_watcher_binding_.NewBinding());

    modular::IntentParameter param1;
    param1.name = "link";
    param1.data.set_link_name("link");

    modular::IntentParameter param2;
    param2.name = nullptr;
    param2.data.set_link_name(nullptr);

    modular::Intent intent;
    intent.action.handler = kModule2Url;
    intent.parameters.push_back(std::move(param1));
    intent.parameters.push_back(std::move(param2));

    module_host_->module_context()->StartModule(
        "two", std::move(intent),
        nullptr /* incoming_services */,
        module_controller_.NewRequest(),
        nullptr /* surface_relation */,
        [](modular::StartModuleStatus){});
  }

  // Called from ModuleDriver.
  void Terminate(const std::function<void()>& done) {
    modular::testing::GetStore()->Put("module1_stop", "", [] {});
    modular::testing::Done(done);
  }

 private:
  TestPoint link1_check_{"Link1"};
  TestPoint link2_check_{"Link2"};

  bool link1_checked_{};
  bool link2_checked_{};

  // |LinkWatcher|
  void Notify(fidl::StringPtr json) override {
    // This watches both link1 and link2. We distinguish the two by the value
    // received.
    FXL_LOG(INFO) << "module1 link: " << json;

    if (json == "1") {
      link1_check_.Pass();
      link1_checked_ = true;
    }

    if (json == "2") {
      link2_check_.Pass();
      link2_checked_ = true;
    }

    if (link1_checked_ && link2_checked_) {
      module_host_->module_context()->Done();
    }
  }

  modular::ModuleHost* const module_host_;
  modular::LinkPtr link1_;
  fidl::Binding<modular::LinkWatcher> link1_watcher_binding_;
  modular::LinkPtr link2_;
  fidl::Binding<modular::LinkWatcher> link2_watcher_binding_;
  modular::ModuleControllerPtr module_controller_;
};

}  // namespace

int main(int /*argc*/, const char** /*argv*/) {
  fsl::MessageLoop loop;
  auto app_context = component::ApplicationContext::CreateFromStartupInfo();
  modular::ModuleDriver<TestApp> driver(app_context.get(),
                                         [&loop] { loop.QuitNow(); });
  loop.Run();
  return 0;
}
