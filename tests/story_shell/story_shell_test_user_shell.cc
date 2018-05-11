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
#include "peridot/tests/common/defs.h"
#include "peridot/tests/story_shell/defs.h"

using modular::testing::TestPoint;
using modular::testing::Get;
using modular::testing::Put;

namespace {

// Cf. README.md for what this test does and how.
class TestApp : public modular::testing::ComponentBase<modular::UserShell> {
 public:
  explicit TestApp(component::ApplicationContext* const application_context)
      : ComponentBase(application_context) {
    TestInit(__FILE__);
  }

  ~TestApp() override = default;

 private:
  TestPoint create_view_{"CreateView()"};

  // |SingleServiceApp|
  void CreateView(
      fidl::InterfaceRequest<views_v1_token::ViewOwner> /*view_owner_request*/,
      fidl::InterfaceRequest<component::ServiceProvider> /*services*/)
      override {
    create_view_.Pass();
  }

  // |UserShell|
  void Initialize(fidl::InterfaceHandle<modular::UserShellContext>
                      user_shell_context) override {
    user_shell_context_.Bind(std::move(user_shell_context));
    user_shell_context_->GetStoryProvider(story_provider_.NewRequest());

    Story1_Create();
  }

  TestPoint story1_create_{"Story1 Create"};

  void Story1_Create() {
    story_provider_->CreateStory(kCommonNullModule,
                                 [this](fidl::StringPtr story_id) {
                                   story1_create_.Pass();
                                   Story1_Run1(story_id);
                                 });
  }

  TestPoint story1_run1_{"Story1 Run1"};

  void Story1_Run1(fidl::StringPtr story_id) {
    auto proceed_after_5 = modular::testing::NewBarrierClosure(5, [this] {
        story1_run1_.Pass();
        Story1_Stop1();
      });

    Get("root:one", proceed_after_5);
    Get("root:one manifest", proceed_after_5);
    Get("root:one:two", proceed_after_5);
    Get("root:one:two manifest", proceed_after_5);
    Get("root:one:two ordering", proceed_after_5);

    story_provider_->GetController(story_id, story_controller_.NewRequest());

    fidl::InterfaceHandle<views_v1_token::ViewOwner> story_view;
    story_controller_->Start(story_view.NewRequest());

    // TODO(mesch): StoryController.AddModule() with a null parent module loses
    // information about the order in which modules are added. When the story is
    // resumed, external modules without parent modules are started in
    // alphabetical order of their names, not in the order they were added to
    // the story.
    fidl::VectorPtr<fidl::StringPtr> parent_one;
    parent_one.push_back("root");
    modular::Intent intent_one;
    intent_one.action.name = kCommonNullAction;
    story_controller_->AddModule(std::move(parent_one), "one",
                                 std::move(intent_one),
                                 nullptr /* surface_relation */);

    fidl::VectorPtr<fidl::StringPtr> parent_two;
    parent_two.push_back("root");
    parent_two.push_back("one");
    modular::Intent intent_two;
    intent_two.action.name = kCommonNullAction;
    story_controller_->AddModule(std::move(parent_two), "two",
                                 std::move(intent_two),
                                 nullptr /* surface_relation */);
  }

  void Story1_Stop1() {
    story_controller_->Stop([this] {
        Story1_Run2();
      });
  }

  TestPoint story1_run2_{"Story1 Run2"};

  void Story1_Run2() {
    auto proceed_after_5 = modular::testing::NewBarrierClosure(5, [this] {
        story1_run2_.Pass();
        Story1_Stop2();
      });

    Get("root:one", proceed_after_5);
    Get("root:one manifest", proceed_after_5);
    Get("root:one:two", proceed_after_5);
    Get("root:one:two manifest", proceed_after_5);
    Get("root:one:two ordering", proceed_after_5);

    fidl::InterfaceHandle<views_v1_token::ViewOwner> story_view;
    story_controller_->Start(story_view.NewRequest());
  }

  void Story1_Stop2() {
    story_controller_->Stop([this] {
        Story2_Create();
      });
  }

  // We do the same sequence with Story2 that we did for Story1, except that the
  // modules are started with packages rather than actions in their Intents.

  TestPoint story2_create_{"Story2 Create"};

  void Story2_Create() {
    story_provider_->CreateStory(kCommonNullModule,
                                 [this](fidl::StringPtr story_id) {
                                   story2_create_.Pass();
                                   Story2_Run1(story_id);
                                 });
  }

  TestPoint story2_run1_{"Story2 Run1"};

  void Story2_Run1(fidl::StringPtr story_id) {
    auto proceed_after_5 = modular::testing::NewBarrierClosure(5, [this] {
        story2_run1_.Pass();
        Story2_Stop1();
      });

    Get("root:one", proceed_after_5);
    Get("root:one manifest", proceed_after_5);
    Get("root:one:two", proceed_after_5);
    Get("root:one:two manifest", proceed_after_5);
    Get("root:one:two ordering", proceed_after_5);

    story_provider_->GetController(story_id, story_controller_.NewRequest());

    fidl::InterfaceHandle<views_v1_token::ViewOwner> story_view;
    story_controller_->Start(story_view.NewRequest());

    fidl::VectorPtr<fidl::StringPtr> parent_one;
    parent_one.push_back("root");
    modular::Intent intent_one;
    intent_one.action.handler = kCommonNullModule;
    story_controller_->AddModule(std::move(parent_one), "one",
                                 std::move(intent_one),
                                 nullptr /*surface_relation) */);

    fidl::VectorPtr<fidl::StringPtr> parent_two;
    parent_two.push_back("root");
    parent_two.push_back("one");
    modular::Intent intent_two;
    intent_two.action.handler = kCommonNullModule;
    story_controller_->AddModule(std::move(parent_two), "two",
                                 std::move(intent_two),
                                 nullptr /* surface_relation */);
  }

  void Story2_Stop1() {
    story_controller_->Stop([this] {
        Story2_Run2();
      });
  }

  TestPoint story2_run2_{"Story2 Run2"};

  void Story2_Run2() {
    auto proceed_after_5 = modular::testing::NewBarrierClosure(5, [this] {
        story2_run2_.Pass();
        Story2_Stop2();
      });

    Get("root:one", proceed_after_5);
    Get("root:one manifest", proceed_after_5);
    Get("root:one:two", proceed_after_5);
    Get("root:one:two manifest", proceed_after_5);
    Get("root:one:two ordering", proceed_after_5);

    fidl::InterfaceHandle<views_v1_token::ViewOwner> story_view;
    story_controller_->Start(story_view.NewRequest());
  }

  void Story2_Stop2() {
    story_controller_->Stop([this] {
        user_shell_context_->Logout();
      });
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
