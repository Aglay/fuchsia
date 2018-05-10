// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <string>

#include <fuchsia/cpp/modular.h>
#include <fuchsia/cpp/views_v1_token.h>
#include "lib/app/cpp/application_context.h"
#include "lib/fidl/cpp/binding.h"
#include "lib/fxl/logging.h"
#include "lib/fxl/macros.h"
#include "peridot/lib/common/story_provider_watcher_base.h"
#include "peridot/lib/testing/component_base.h"
#include "peridot/lib/testing/testing.h"
#include "peridot/tests/common/defs.h"
#include "peridot/tests/last_focus_time/defs.h"

namespace {

// A simple story provider watcher implementation. It confirms that it sees an
// increase in the last_focus_time in the StoryInfo it receives, and pushes the
// test through to the next step.
class StoryProviderWatcherImpl : public modular::StoryProviderWatcherBase {
 public:
  StoryProviderWatcherImpl() = default;
  ~StoryProviderWatcherImpl() override = default;

 private:
  using TestPoint = modular::testing::TestPoint;

  TestPoint last_focus_time_created_{
      "StoryInfo::last_focus_time increased after create"};
  TestPoint last_focus_time_focused_{
      "StoryInfo::last_focus_time increased after focus"};

  // |StoryProviderWatcher|
  void OnChange(modular::StoryInfo story_info,
                modular::StoryState story_state) override {
    FXL_CHECK(story_info.last_focus_time >= last_focus_time_);
    if (story_info.last_focus_time <= last_focus_time_) {
      return;
    }

    // Every time we see an increase in last_focus_time, we push the test
    // sequence forward.
    //
    // We expect two last_focus_time transitions:
    //
    //   0 -> X on creation of the story.
    //
    //   X -> Y where Y > X on focusing the story.
    //
    switch (++change_count_) {
      case 1:
        last_focus_time_created_.Pass();
        break;
      case 2:
        last_focus_time_focused_.Pass();
        break;
      default:
        FXL_CHECK(change_count_ == 1 || change_count_ == 2);
        break;
    }

    last_focus_time_ = story_info.last_focus_time;
    continue_();
  }

  int change_count_{};
  int64_t last_focus_time_{};
};

class StoryWatcherImpl : modular::StoryWatcher {
 public:
  StoryWatcherImpl() : binding_(this) {}
  ~StoryWatcherImpl() override = default;

  // Registers itself as a watcher on the given story. Only one story at a time
  // can be watched.
  void Watch(modular::StoryController* const story_controller) {
    story_controller->Watch(binding_.NewBinding());
  }

  // Deregisters itself from the watched story.
  void Reset() { binding_.Unbind(); }

  // Sets the function where to continue when the story is observed to be
  // running.
  void Continue(std::function<void()> at) { continue_ = at; }

 private:
  // |StoryWatcher|
  void OnStateChange(modular::StoryState state) override {
    FXL_LOG(INFO) << "OnStateChange() " << state;
    if (state != modular::StoryState::RUNNING) {
      return;
    }

    continue_();
  }

  // |StoryWatcher|
  void OnModuleAdded(modular::ModuleData /*module_data*/) override {}

  fidl::Binding<modular::StoryWatcher> binding_;
  std::function<void()> continue_;
  FXL_DISALLOW_COPY_AND_ASSIGN(StoryWatcherImpl);
};

// A simple focus watcher implementation that invokes a "continue" callback when
// it sees the next focus change.
class FocusWatcherImpl : modular::FocusWatcher {
 public:
  FocusWatcherImpl() : binding_(this) {}
  ~FocusWatcherImpl() override = default;

  // Registers itself as a watcher on the focus provider.
  void Watch(modular::FocusProvider* const focus_provider) {
    focus_provider->Watch(binding_.NewBinding());
  }

  // Deregisters itself from the watched focus provider.
  void Reset() { binding_.Unbind(); }

 private:
  // |FocusWatcher|
  void OnFocusChange(modular::FocusInfoPtr info) override {
    FXL_LOG(INFO) << "OnFocusChange() " << info->focused_story_id;
  }

  fidl::Binding<modular::FocusWatcher> binding_;
  FXL_DISALLOW_COPY_AND_ASSIGN(FocusWatcherImpl);
};

// Cf. README.md for what this test does and how.
class TestApp : public modular::testing::ComponentBase<modular::UserShell> {
 public:
  TestApp(component::ApplicationContext* const application_context)
      : ComponentBase(application_context) {
    TestInit(__FILE__);
  }

  ~TestApp() override = default;

 private:
  using TestPoint = modular::testing::TestPoint;

  TestPoint initialize_{"Initialize()"};

  // |UserShell|
  void Initialize(fidl::InterfaceHandle<modular::UserShellContext>
                  user_shell_context) override {
    initialize_.Pass();

    user_shell_context_.Bind(std::move(user_shell_context));
    user_shell_context_->GetStoryProvider(story_provider_.NewRequest());
    story_provider_watcher_.Watch(&story_provider_);

    user_shell_context_->GetFocusController(focus_controller_.NewRequest());
    user_shell_context_->GetFocusProvider(focus_provider_.NewRequest());
    focus_watcher_.Watch(focus_provider_.get());

    CreateStory();
  }

  TestPoint create_story_{"CreateStory()"};

  void CreateStory() {
    story_provider_->CreateStory(kCommonNullModule,
                                 [this](const fidl::StringPtr& story_id) {
                                   create_story_.Pass();
                                   story_id_ = story_id;
                                   StartStory();
                                 });
  }

  TestPoint start_story_{"StartStory()"};

  void StartStory() {
    story_provider_->GetController(story_id_, story_controller_.NewRequest());
    story_watcher_.Watch(story_controller_.get());

    // Start and show the new story.
    fidl::InterfaceHandle<views_v1_token::ViewOwner> story_view;
    story_controller_->Start(story_view.NewRequest());

    story_watcher_.Continue([this] {
      start_story_.Pass();
      Focus();
    });
  }

  TestPoint focus_{"Focus()"};

  void Focus() {
    focus_controller_->Set(story_id_);

    story_provider_watcher_.Continue([this] {
      focus_.Pass();
      Logout();
    });
  }

  void Logout() {
    story_provider_watcher_.Reset();
    user_shell_context_->Logout();
  }

  modular::UserShellContextPtr user_shell_context_;

  modular::StoryProviderPtr story_provider_;
  StoryProviderWatcherImpl story_provider_watcher_;

  fidl::StringPtr story_id_;
  modular::StoryControllerPtr story_controller_;
  StoryWatcherImpl story_watcher_;

  modular::FocusControllerPtr focus_controller_;
  modular::FocusProviderPtr focus_provider_;
  FocusWatcherImpl focus_watcher_;

  FXL_DISALLOW_COPY_AND_ASSIGN(TestApp);
};

}  // namespace

int main(int /*argc*/, const char** /*argv*/) {
  modular::testing::ComponentMain<TestApp>();
  return 0;
}
