// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <sstream>
#include <string>

#include <fuchsia/cpp/modular.h>
#include <fuchsia/cpp/views_v1_token.h>

#include "lib/app/cpp/application_context.h"
#include "lib/context/cpp/formatting.h"
#include "lib/fidl/cpp/binding.h"
#include "lib/fxl/logging.h"
#include "lib/fxl/macros.h"
#include "peridot/lib/fidl/array_to_string.h"
#include "peridot/lib/rapidjson/rapidjson.h"
#include "peridot/lib/testing/component_base.h"
#include "peridot/lib/testing/reporting.h"
#include "peridot/lib/testing/testing.h"

namespace modular {
namespace {

constexpr char kModuleUrl[] =
    "file:///system/test/modular_tests/chain_test_module";

// Tests starting Modules with a Daisy and the subsequent initialization of the
// Module's Links based on the values of Daisy.nouns.
class TestApp : public testing::ComponentBase<UserShell>,
                StoryWatcher,
                ModuleWatcher {
 public:
  TestApp(component::ApplicationContext* const application_context)
      : ComponentBase(application_context),
        story_watcher_binding_(this),
        module_watcher_binding_(this) {
    TestInit(__FILE__);
  }

  ~TestApp() override = default;

 private:
  using TestPoint = testing::TestPoint;

  TestPoint initialize_{"Initialize()"};

  // |UserShell|
  void Initialize(
      fidl::InterfaceHandle<UserShellContext> user_shell_context) override {
    initialize_.Pass();
    user_shell_context_.Bind(std::move(user_shell_context));
    user_shell_context_->GetStoryProvider(story_provider_.NewRequest());

    CreateStory();
  }

  // |StoryWatcher|
  void OnStateChange(StoryState state) override {
    if (state == StoryState::DONE)
      Logout();
  }

  // |ModuleWatcher|
  void OnStateChange(ModuleState state) override {
    if (state == ModuleState::DONE) {
      // When our child Module exits, we should exit.
      // child_module_->Stop([this] { module_context_->Done(); });
      child_module_->Stop([] {});
    }
  }

  // |StoryWatcher|
  void OnModuleAdded(ModuleData module_data) override {}

  TestPoint create_story_{"CreateStory()"};

  void CreateStory() {
    // Create an empty Story. Once it has been created, add our first Module.
    story_provider_->CreateStory(
        nullptr /* module_url */, [this](const fidl::StringPtr& story_id) {
          create_story_.Pass();
          story_id_ = story_id;
          story_provider_->GetController(story_id_,
                                         story_controller_.NewRequest());
          AddRootModule();
        });
  }

  void AddRootModule() {
    Daisy daisy;
    daisy.url = kModuleUrl;

    Noun noun;
    noun.set_json(R"("initial data for the story")");
    NounEntry entry;
    entry.name = "rootModuleNoun1";
    entry.noun = std::move(noun);
    daisy.nouns.push_back(std::move(entry));
    story_controller_->AddModule({}, "rootMod", std::move(daisy),
                                 nullptr /* surface_relation */);
    fidl::VectorPtr<fidl::StringPtr> path;
    path.reset({"rootMod"});
    story_controller_->GetModuleController(std::move(path),
                                           child_module_.NewRequest());
    ModuleWatcherPtr watcher;
    module_watcher_binding_.Bind(watcher.NewRequest());
    child_module_->Watch(std::move(watcher));

    StartStory();
  }

  void StartStory() {
    // Start and show the new story.
    fidl::InterfaceRequest<views_v1_token::ViewOwner> story_view;
    story_controller_->Start(std::move(story_view));

    StoryWatcherPtr watcher;
    story_watcher_binding_.Bind(watcher.NewRequest());
    story_controller_->Watch(std::move(watcher));
  }

  void Logout() { user_shell_context_->Logout(); }

  UserShellContextPtr user_shell_context_;
  StoryProviderPtr story_provider_;

  fidl::StringPtr story_id_;
  StoryControllerPtr story_controller_;

  ModuleControllerPtr child_module_;

  fidl::Binding<StoryWatcher> story_watcher_binding_;
  fidl::Binding<ModuleWatcher> module_watcher_binding_;

  FXL_DISALLOW_COPY_AND_ASSIGN(TestApp);
};

}  // namespace
}  // namespace modular

int main(int /*argc*/, const char** /*argv*/) {
  modular::testing::ComponentMain<modular::TestApp>();
  return 0;
}
