// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <iostream>

#include "garnet/lib/callback/scoped_callback.h"
#include "lib/app_driver/cpp/module_driver.h"
#include "lib/fsl/tasks/message_loop.h"
#include "lib/fxl/memory/weak_ptr.h"
#include "lib/module/fidl/module.fidl.h"
#include "lib/ui/views/fidl/view_token.fidl.h"
#include "peridot/lib/testing/reporting.h"
#include "peridot/lib/testing/testing.h"

using modular::testing::TestPoint;

namespace {

constexpr int kTimeoutMilliseconds = 5000;

constexpr char kChildModuleName[] = "child";
constexpr char kChildModuleUrl[] =
    "file:///system/test/modular_tests/parent_child_test_child_module";

class ParentApp {
 public:
  ParentApp(
      modular::ModuleHost* module_host,
      f1dl::InterfaceRequest<mozart::ViewProvider> /*view_provider_request*/,
      f1dl::InterfaceRequest<component::ServiceProvider> /*outgoing_services*/)
      : module_host_(module_host), weak_ptr_factory_(this) {
    modular::testing::Init(module_host->application_context(), __FILE__);
    initialized_.Pass();

    // Start a timer to quit in case another test component misbehaves and we
    // time out.
    fsl::MessageLoop::GetCurrent()->task_runner()->PostDelayedTask(
        callback::MakeScoped(
            weak_ptr_factory_.GetWeakPtr(),
            [this] { module_host_->module_context()->Done(); }),
        fxl::TimeDelta::FromMilliseconds(kTimeoutMilliseconds));

    StartChildModuleTwice();
  }

  // Called by ModuleDriver.
  void Terminate(const std::function<void()>& done) {
    stopped_.Pass();
    modular::testing::Done(done);
  }

 private:
  void StartChildModuleTwice() {
    auto daisy = modular::Daisy::New();
    daisy->url = kChildModuleUrl;
    auto noun_entry = modular::NounEntry::New();
    noun_entry->name = "link";
    noun_entry->noun = modular::Noun::New();
    noun_entry->noun->set_link_name("module1link");
    daisy->nouns.push_back(std::move(noun_entry));
    module_host_->module_context()->StartModule(
        kChildModuleName, std::move(daisy), nullptr, child_module_.NewRequest(),
        nullptr, [](const modular::StartModuleStatus) {});

    // Once the module starts, start the same module again, but with a different
    // link mapping. This stops the previous module instance and starts a new one.
    modular::testing::GetStore()->Get(
        "child_module_init", [this](const f1dl::StringPtr&) {
          child_module_.set_error_handler([this] { OnChildModuleStopped(); });

          auto daisy = modular::Daisy::New();
          daisy->url = kChildModuleUrl;
          auto noun_entry = modular::NounEntry::New();
          noun_entry->name = "link";
          noun_entry->noun = modular::Noun::New();
          noun_entry->noun->set_link_name("module2link");
          daisy->nouns.push_back(std::move(noun_entry));
          module_host_->module_context()->StartModule(
              kChildModuleName, std::move(daisy), nullptr,
              child_module2_.NewRequest(), nullptr,
              [](const modular::StartModuleStatus) {});
        });
  }

  TestPoint child_module_down_{"Child module killed for restart"};

  void OnChildModuleStopped() {
    child_module_down_.Pass();

    // Confirm that the first module instance stopped, and then stop the second
    // module instance.
    modular::testing::GetStore()->Get(
        "child_module_stop", [this](const f1dl::StringPtr&) {
          child_module2_->Stop([this] { OnChildModule2Stopped(); });
        });
  }

  TestPoint child_module_stopped_{"Child module stopped"};

  void OnChildModule2Stopped() {
    child_module_stopped_.Pass();
    module_host_->module_context()->Done();
  }

  TestPoint initialized_{"Parent module initialized"};
  TestPoint stopped_{"Parent module stopped"};

  modular::ModuleHost* module_host_;
  modular::ModuleControllerPtr child_module_;
  modular::ModuleControllerPtr child_module2_;

  fxl::WeakPtrFactory<ParentApp> weak_ptr_factory_;
};

}  // namespace

int main(int /*argc*/, const char** /*argv*/) {
  fsl::MessageLoop loop;
  auto app_context = component::ApplicationContext::CreateFromStartupInfo();
  modular::ModuleDriver<ParentApp> driver(app_context.get(),
                                          [&loop] { loop.QuitNow(); });
  loop.Run();
  return 0;
}
