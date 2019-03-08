// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <string>

#include <fuchsia/modular/cpp/fidl.h>
#include <lib/component/cpp/startup_context.h>
#include <lib/fidl/cpp/binding.h>
#include <lib/fxl/logging.h>
#include <lib/fxl/macros.h>

#include "peridot/lib/testing/component_main.h"
#include "peridot/lib/testing/session_shell_base.h"
#include "peridot/public/lib/integration_testing/cpp/testing.h"
#include "peridot/tests/common/defs.h"
#include "peridot/tests/last_focus_time/defs.h"

using modular::testing::Signal;
using modular::testing::TestPoint;

namespace {

constexpr char kStoryName[] = "story1";

// A simple story provider watcher implementation. It confirms that it sees an
// increase in the last_focus_time in the fuchsia::modular::StoryInfo it
// receives, and pushes the test through to the next step.
class StoryProviderWatcherImpl : public fuchsia::modular::StoryProviderWatcher {
 public:
  StoryProviderWatcherImpl() : continue_([] {}), binding_(this) {}
  ~StoryProviderWatcherImpl() override = default;

  void Continue(std::function<void()> at) { continue_ = std::move(at); }

  void Watch(fuchsia::modular::StoryProvider* const story_provider) {
    story_provider->Watch(binding_.NewBinding());
  }

  void Reset() { binding_.Unbind(); }

 private:
  TestPoint last_focus_time_created_{
      "StoryInfo::last_focus_time increased after create"};
  TestPoint last_focus_time_focused_{
      "StoryInfo::last_focus_time increased after focus"};

  // |fuchsia::modular::StoryProviderWatcher|
  void OnDelete(::std::string story_id) override {}

  // |fuchsia::modular::StoryProviderWatcher|
  void OnChange(
      fuchsia::modular::StoryInfo story_info,
      fuchsia::modular::StoryState story_state,
      fuchsia::modular::StoryVisibilityState story_visibility_state) override {
    FXL_CHECK(story_info.last_focus_time >= last_focus_time_);
    if (story_info.last_focus_time <= last_focus_time_) {
      return;
    }

    // Every time we see an increase in last_focus_time, we push the test
    // sequence forward.
    //
    // We expect two last_focus_time transitions:
    //
    //   -1 -> 0 on creation of the story.
    //
    //   0 -> Y where Y > 0 on focusing the story.
    //
    switch (++change_count_) {
      case 1:
        if (story_info.last_focus_time == 0) {
          last_focus_time_created_.Pass();
        }
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
  int64_t last_focus_time_{-1};

  std::function<void()> continue_;
  fidl::Binding<fuchsia::modular::StoryProviderWatcher> binding_;
};

class StoryWatcherImpl : fuchsia::modular::StoryWatcher {
 public:
  StoryWatcherImpl() : binding_(this) {}
  ~StoryWatcherImpl() override = default;

  // Registers itself as a watcher on the given story. Only one story at a time
  // can be watched.
  void Watch(fuchsia::modular::StoryController* const story_controller) {
    story_controller->Watch(binding_.NewBinding());
  }

  // Deregisters itself from the watched story.
  void Reset() { binding_.Unbind(); }

  // Sets the function where to continue when the story is observed to be
  // running.
  void Continue(std::function<void()> at) { continue_ = at; }

 private:
  // |fuchsia::modular::StoryWatcher|
  void OnStateChange(fuchsia::modular::StoryState state) override {
    FXL_LOG(INFO) << "OnStateChange() " << fidl::ToUnderlying(state);
    if (state != fuchsia::modular::StoryState::RUNNING) {
      return;
    }

    continue_();
  }

  // |fuchsia::modular::StoryWatcher|
  void OnModuleAdded(fuchsia::modular::ModuleData /*module_data*/) override {}

  // |fuchsia::modular::StoryWatcher|
  void OnModuleFocused(std::vector<std::string> /*module_path*/) override {}

  fidl::Binding<fuchsia::modular::StoryWatcher> binding_;
  std::function<void()> continue_;
  FXL_DISALLOW_COPY_AND_ASSIGN(StoryWatcherImpl);
};

// A simple focus watcher implementation that invokes a "continue" callback when
// it sees the next focus change.
class FocusWatcherImpl : fuchsia::modular::FocusWatcher {
 public:
  FocusWatcherImpl() : binding_(this) {}
  ~FocusWatcherImpl() override = default;

  // Registers itself as a watcher on the focus provider.
  void Watch(fuchsia::modular::FocusProvider* const focus_provider) {
    focus_provider->Watch(binding_.NewBinding());
  }

  // Deregisters itself from the watched focus provider.
  void Reset() { binding_.Unbind(); }

 private:
  // |fuchsia::modular::FocusWatcher|
  void OnFocusChange(fuchsia::modular::FocusInfoPtr info) override {
    FXL_LOG(INFO) << "OnFocusChange() " << info->focused_story_id;
  }

  fidl::Binding<fuchsia::modular::FocusWatcher> binding_;

  FXL_DISALLOW_COPY_AND_ASSIGN(FocusWatcherImpl);
};

// Cf. README.md for what this test does and how.
class TestApp : public modular::testing::SessionShellBase {
 public:
  TestApp(component::StartupContext* const startup_context)
      : SessionShellBase(startup_context) {
    TestInit(__FILE__);

    startup_context->ConnectToEnvironmentService(puppet_master_.NewRequest());
    story_provider_watcher_.Watch(story_provider());

    session_shell_context()->GetFocusController(focus_controller_.NewRequest());
    session_shell_context()->GetFocusProvider(focus_provider_.NewRequest());
    focus_watcher_.Watch(focus_provider_.get());

    CreateStory();
  }

  ~TestApp() override = default;

 private:
  TestPoint create_story_{"CreateStory()"};

  void CreateStory() {
    puppet_master_->ControlStory(kStoryName, story_puppet_master_.NewRequest());

    fuchsia::modular::AddMod add_mod;
    add_mod.mod_name_transitional = "mod1";
    add_mod.intent.handler = kCommonNullModule;
    add_mod.intent.action = kCommonNullAction;

    std::vector<fuchsia::modular::StoryCommand> commands(1);
    commands.at(0).set_add_mod(std::move(add_mod));

    story_puppet_master_->Enqueue(std::move(commands));
    story_puppet_master_->Execute(
        [this](fuchsia::modular::ExecuteResult result) {
          create_story_.Pass();
          StartStory();
        });
  }

  TestPoint start_story_{"StartStory()"};

  void StartStory() {
    story_provider()->GetController(kStoryName, story_controller_.NewRequest());
    story_watcher_.Watch(story_controller_.get());

    // Request start of the new story.
    story_controller_->RequestStart();

    story_watcher_.Continue([this] {
      start_story_.Pass();
      story_watcher_.Reset();
      Focus();
    });
  }

  TestPoint focus_{"Focus()"};

  void Focus() {
    focus_controller_->Set(kStoryName);

    story_provider_watcher_.Continue([this] {
      focus_.Pass();
      story_provider_watcher_.Reset();

      Signal(modular::testing::kTestShutdown);
    });
  }

  fuchsia::modular::PuppetMasterPtr puppet_master_;
  fuchsia::modular::StoryPuppetMasterPtr story_puppet_master_;
  StoryProviderWatcherImpl story_provider_watcher_;

  fuchsia::modular::StoryControllerPtr story_controller_;
  StoryWatcherImpl story_watcher_;

  fuchsia::modular::FocusControllerPtr focus_controller_;
  fuchsia::modular::FocusProviderPtr focus_provider_;
  FocusWatcherImpl focus_watcher_;

  FXL_DISALLOW_COPY_AND_ASSIGN(TestApp);
};

}  // namespace

int main(int /*argc*/, const char** /*argv*/) {
  modular::testing::ComponentMain<TestApp>();
  return 0;
}
