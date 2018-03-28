// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/cpp/modular.h>
#include <fuchsia/cpp/views_v1.h>
#include <fuchsia/cpp/views_v1_token.h>

#include "lib/app_driver/cpp/module_driver.h"
#include "lib/fsl/tasks/message_loop.h"
#include "lib/fxl/functional/make_copyable.h"
#include "lib/fxl/tasks/task_runner.h"
#include "lib/fxl/time/time_delta.h"
#include "peridot/lib/testing/reporting.h"
#include "peridot/lib/testing/testing.h"

using modular::testing::TestPoint;

namespace modular {
namespace {

constexpr char kChildModuleUrl[] =
    "/system/test/modular_tests/chain_test_child_module";

class TestApp : public ModuleWatcher {
 public:
  TestApp(
      ModuleHost* module_host,
      fidl::InterfaceRequest<views_v1::ViewProvider> /*view_provider_request*/,
      fidl::InterfaceRequest<component::ServiceProvider> /*outgoing_services*/)
      : module_context_(module_host->module_context()),
        module_watcher_binding_(this) {
    module_context_->GetComponentContext(component_context_.NewRequest());
    testing::Init(module_host->application_context(), __FILE__);
    initialized_.Pass();

    // We'll use an Entity stored on one of our Links, which will be used in
    // the resolution process to choose a compatible Module.
    // TODO(thatguy): We should be specifying type constraints when we create
    // the Link.
    auto entity_data = fidl::VectorPtr<TypeToDataEntry>::New(1);
    entity_data->at(0) = TypeToDataEntry();
    entity_data->at(0).type = "myType";
    entity_data->at(0).data = "1337";
    component_context_->CreateEntityWithData(
        std::move(entity_data), [this](fidl::StringPtr reference) {
          entity_one_reference_ = reference;
          EmbedModule();
        });
  }

  // Called from ModuleDriver.
  void Terminate(const std::function<void()>& done) {
    stopped_.Pass();
    testing::Done(done);
  }

 private:
  void EmbedModule() {
    Daisy daisy_;
    daisy_.url = kChildModuleUrl;
    daisy_.nouns.resize(3);

    // We'll put three nouns "one", "two" and "three" on the Daisy. The first
    // is used to match the Module, because we know that it expectes a noun
    // named "one". The other two are extra and are going to be passed on
    // to the Module regardless.
    //
    // The second noun is set to a Link that we own with regular JSON content.
    //
    // The third noun we expect to reference a Link created on our behalf by
    // the Framework. We don't get access to that Link.
    module_context_->GetLink("foo", link_one_.NewRequest());
    link_one_->SetEntity(entity_one_reference_);
    Noun noun;
    noun.set_link_name("foo");
    daisy_.nouns->at(0) = NounEntry();
    daisy_.nouns->at(0).name = "one";
    daisy_.nouns->at(0).noun = std::move(noun);

    module_context_->GetLink("bar", link_two_.NewRequest());
    link_two_->Set(nullptr, "12345");
    noun = Noun();
    noun.set_link_name("bar");
    daisy_.nouns->at(1) = NounEntry();
    daisy_.nouns->at(1).name = "two";
    daisy_.nouns->at(1).noun = std::move(noun);

    noun = Noun();
    noun.set_json("67890");
    daisy_.nouns->at(2) = NounEntry();
    daisy_.nouns->at(2).name = "three";
    daisy_.nouns->at(2).noun = std::move(noun);

    // Sync to avoid race conditions between writing
    link_one_->Sync([this, &daisy_] {
      link_two_->Sync([this, &daisy_] {
        module_context_->EmbedModule(
            "my child", std::move(daisy_), nullptr, child_module_.NewRequest(),
            child_view_.NewRequest(), [this](StartModuleStatus status) {
              if (status == StartModuleStatus::SUCCESS) {
                start_daisy_.Pass();
              } else {
                module_context_->Done();
              }
            });
        child_module_.set_error_handler(
            [this]() { child_module_stopped_.Pass(); });
        ModuleWatcherPtr watcher;
        module_watcher_binding_.Bind(watcher.NewRequest());
        child_module_->Watch(std::move(watcher));
      });
    });
  }

  // |ModuleWatcher|
  void OnStateChange(ModuleState state) override {
    if (state == ModuleState::DONE) {
      // When our child Module exits, we should exit.
      child_module_->Stop([this] { module_context_->Done(); });
    }
  }

  ComponentContextPtr component_context_;
  ModuleContext* module_context_;
  ModuleControllerPtr child_module_;
  views_v1_token::ViewOwnerPtr child_view_;

  fidl::StringPtr entity_one_reference_;
  DaisyPtr daisy_;

  LinkPtr link_one_;
  LinkPtr link_two_;

  fidl::Binding<ModuleWatcher> module_watcher_binding_;

  TestPoint start_daisy_{"Started child Daisy"};
  TestPoint child_module_stopped_{"Child module observed to have stopped"};
  TestPoint initialized_{"Parent module initialized"};
  TestPoint stopped_{"Parent module stopped"};
};

}  // namespace
}  // namespace modular

int main(int /*argc*/, const char** /*argv*/) {
  fsl::MessageLoop loop;
  auto app_context = component::ApplicationContext::CreateFromStartupInfo();
  modular::ModuleDriver<modular::TestApp> driver(app_context.get(),
                                                 [&loop] { loop.QuitNow(); });
  loop.Run();
  return 0;
}
