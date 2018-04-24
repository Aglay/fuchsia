// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <set>
#include <utility>

#include <fuchsia/cpp/component.h>
#include <fuchsia/cpp/modular.h>
#include <fuchsia/cpp/views_v1_token.h>
#include <lib/async/cpp/task.h>
#include <lib/async/default.h>

#include "lib/app/cpp/application_context.h"
#include "lib/fidl/cpp/binding.h"
#include "lib/fxl/command_line.h"
#include "lib/fxl/logging.h"
#include "lib/fxl/macros.h"
#include "lib/fxl/tasks/task_runner.h"
#include "lib/fxl/time/time_delta.h"
#include "peridot/lib/fidl/clone.h"
#include "peridot/lib/rapidjson/rapidjson.h"
#include "peridot/lib/testing/component_base.h"
#include "peridot/lib/testing/reporting.h"
#include "peridot/lib/testing/testing.h"

namespace {

constexpr char kNullModule[] = "common_null_module";
constexpr char kNullAction[] = "com.google.fuchsia.common.null";

std::function<void(fidl::StringPtr)> Count(const int limit, std::function<void()> done) {
  return [limit, count = std::make_shared<int>(0), done](fidl::StringPtr value) {
    // Cf. function Put() in story_shell_test_story_shell.cc: The value is the
    // same as the key we wait to Get().
    FXL_LOG(INFO) << "Got: " << value;
    ++*count;
    if (*count == limit) {
      done();
    }
  };
}

// Cf. README.md
class TestApp : public modular::testing::ComponentBase<modular::UserShell> {
 public:
  explicit TestApp(component::ApplicationContext* const application_context)
      : ComponentBase(application_context) {
    TestInit(__FILE__);
  }

  ~TestApp() override = default;

 private:
  using TestPoint = modular::testing::TestPoint;

  TestPoint create_view_{"CreateView()"};

  // |SingleServiceApp|
  void CreateView(
      fidl::InterfaceRequest<views_v1_token::ViewOwner> /*view_owner_request*/,
      fidl::InterfaceRequest<component::ServiceProvider> /*services*/)
      override {
    create_view_.Pass();
  }

  TestPoint story1_create_{"Story1 Create"};

  // |UserShell|
  void Initialize(fidl::InterfaceHandle<modular::UserShellContext>
                      user_shell_context) override {
    user_shell_context_.Bind(std::move(user_shell_context));
    user_shell_context_->GetStoryProvider(story_provider_.NewRequest());

    // TODO(mesch): StoryController.AddModule() is broken when it's called
    // before the story is running. So we start the story with a default module,
    // and then AddDaisy() after starting the story. The story controller calls
    // need A LOT of cleanup.
    story_provider_->CreateStory(kNullModule,
                                 [this](fidl::StringPtr story_id) {
                                   story1_create_.Pass();
                                   Story1_Run(story_id);
                                 });
  }

  TestPoint story1_run_{"Story1 Run"};

  void Story1_Run(fidl::StringPtr story_id) {
    auto check = Count(5, [this] {
        story1_run_.Pass();
        Story1_Stop();
      });

    modular::testing::GetStore()->Get("root:one", check);
    modular::testing::GetStore()->Get("root:one manifest", check);
    modular::testing::GetStore()->Get("root:one:two", check);
    modular::testing::GetStore()->Get("root:one:two manifest", check);
    modular::testing::GetStore()->Get("root:one:two ordering", check);

    story_provider_->GetController(story_id, story_controller_.NewRequest());

    fidl::InterfaceHandle<views_v1_token::ViewOwner> story_view;
    story_controller_->Start(story_view.NewRequest());

    // TODO(mesch): AddModule() with a null surface relation indicates an
    // embedded module. But AddModule() allows a null surface relationship, even
    // though the module is not embedded. Makes no sense.
    modular::SurfaceRelation surface_relation;

    // TODO(mesch): StoryController.AddModule() with a null parent module still
    // picks a random parent. Remove that.
    fidl::VectorPtr<fidl::StringPtr> parent_one;
    parent_one.push_back("root");
    modular::Intent intent_one;
    intent_one.action.name = kNullAction;
    story_controller_->AddModule(std::move(parent_one), "one",
                                 std::move(intent_one),
                                 modular::CloneOptional(surface_relation));

    fidl::VectorPtr<fidl::StringPtr> parent_two;
    parent_two.push_back("root");
    parent_two.push_back("one");
    modular::Intent intent_two;
    intent_two.action.name = kNullAction;
    story_controller_->AddModule(std::move(parent_two), "two",
                                 std::move(intent_two),
                                 modular::CloneOptional(surface_relation));
  }

  void Story1_Stop() {
    story_controller_->Stop([this] {
        Story2_Run();
      });
  }

  TestPoint story2_run_{"Story2 Run"};

  void Story2_Run() {
    auto check = Count(5, [this] {
        story2_run_.Pass();
        user_shell_context_->Logout();
      });

    modular::testing::GetStore()->Get("root:one", check);
    modular::testing::GetStore()->Get("root:one manifest", check);
    modular::testing::GetStore()->Get("root:one:two", check);
    modular::testing::GetStore()->Get("root:one:two manifest", check);
    modular::testing::GetStore()->Get("root:one:two ordering", check);

    fidl::InterfaceHandle<views_v1_token::ViewOwner> story_view;
    story_controller_->Start(story_view.NewRequest());
  }

  modular::UserShellContextPtr user_shell_context_;
  modular::StoryProviderPtr story_provider_;
  modular::StoryControllerPtr story_controller_;

  FXL_DISALLOW_COPY_AND_ASSIGN(TestApp);
};

}  // namespace

int main(int /* argc */, const char** /* argv */) {
  modular::testing::ComponentMain<TestApp>();
  return 0;
}
