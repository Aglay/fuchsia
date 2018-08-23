// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>

#include <fuchsia/modular/cpp/fidl.h>
#include <fuchsia/ui/viewsv1token/cpp/fidl.h>
#include <lib/callback/scoped_callback.h>
#include <lib/component/cpp/connect.h>
#include <lib/component/cpp/startup_context.h>
#include <lib/fidl/cpp/binding.h>
#include <lib/fsl/vmo/strings.h>
#include <lib/fxl/logging.h>
#include <lib/fxl/macros.h>

#include "peridot/lib/rapidjson/rapidjson.h"
#include "peridot/lib/testing/component_base.h"
#include "peridot/lib/testing/reporting.h"
#include "peridot/lib/testing/testing.h"
#include "peridot/tests/common/defs.h"
#include "peridot/tests/module_context/defs.h"

using ::modular::testing::Await;
using ::modular::testing::Signal;
using ::modular::testing::TestPoint;

namespace {

// A simple story activity watcher implementation.
class StoryActivityWatcherImpl : fuchsia::modular::StoryActivityWatcher {
 public:
  StoryActivityWatcherImpl()
      : binding_(this),
        on_notify_(
            [](fidl::StringPtr,
               fidl::VectorPtr<fuchsia::modular::OngoingActivityType>) {}) {}
  ~StoryActivityWatcherImpl() override = default;

  void Watch(fuchsia::modular::StoryProviderPtr* const story_provider) {
    (*story_provider)->WatchActivity(binding_.NewBinding());
  }

  void OnNotify(std::function<
                void(fidl::StringPtr,
                     fidl::VectorPtr<fuchsia::modular::OngoingActivityType>)>
                    on_notify) {
    on_notify_ = std::move(on_notify);
  }

 private:
  // |fuchsia::modular::StoryActivityWatcher|
  void OnStoryActivityChange(
      fidl::StringPtr story_id,
      fidl::VectorPtr<fuchsia::modular::OngoingActivityType> activities)
      override {
    on_notify_(std::move(story_id), std::move(activities));
  }

  fidl::Binding<fuchsia::modular::StoryActivityWatcher> binding_;
  std::function<void(fidl::StringPtr,
                     fidl::VectorPtr<fuchsia::modular::OngoingActivityType>)>
      on_notify_;

  FXL_DISALLOW_COPY_AND_ASSIGN(StoryActivityWatcherImpl);
};

class TestApp
    : public modular::testing::ComponentBase<fuchsia::modular::UserShell> {
 public:
  TestApp(component::StartupContext* const startup_context)
      : ComponentBase(startup_context), weak_ptr_factory_(this) {
    TestInit(__FILE__);
  }

  ~TestApp() override = default;

 private:
  using TestPoint = modular::testing::TestPoint;

  TestPoint initialize_{"Initialize()"};
  TestPoint story_create_{"Created story."};

  // |fuchsia::modular::UserShell|
  void Initialize(fidl::InterfaceHandle<fuchsia::modular::UserShellContext>
                      user_shell_context) override {
    initialize_.Pass();

    user_shell_context_.Bind(std::move(user_shell_context));
    user_shell_context_->GetStoryProvider(story_provider_.NewRequest());

    story_provider_->CreateStory(nullptr, [this](fidl::StringPtr story_id) {
      story_create_.Pass();
      story_id_ = story_id;
      StartStory(story_id);
    });
    async::PostDelayedTask(
        async_get_default_dispatcher(),
        callback::MakeScoped(weak_ptr_factory_.GetWeakPtr(),
                             [this] { user_shell_context_->Logout(); }),
        zx::msec(modular::testing::kTestTimeoutMilliseconds));
  }

  // Starts the story and adds two modules to it.
  void StartStory(const std::string& story_id) {
    story_provider_->GetController(story_id, story_controller_.NewRequest());
    story_controller_.set_error_handler([this, story_id] {
      FXL_LOG(ERROR) << "Story controller for story " << story_id
                     << " died. Does this story exist?";
    });

    story_controller_->AddModule(nullptr /* parent_module_path */,
                                 kFirstModuleName,
                                 IntentWithParameterString(kFirstModuleName),
                                 nullptr /* surface_relation */);
    story_controller_->AddModule(nullptr /* parent_module_path */,
                                 kSecondModuleName,
                                 IntentWithParameterString(kSecondModuleName),
                                 nullptr /* surface_relation */);

    story_controller_->Start(story_view_.NewRequest());

    PerformFirstModuleStartActivity();
  }

  TestPoint on_watch_ongoing_activities_dispatched{
      "When a watcher is registered, ongoing activities should be dispatched."};
  // Signals the first module to call ModuleContext.StartOngoingActivity().
  void PerformFirstModuleStartActivity() {
    Signal(kFirstModuleCallStartActivity);
    story_activity_watcher_.Watch(&story_provider_);
    story_activity_watcher_.OnNotify(
        [this](
            fidl::StringPtr story_id,
            fidl::VectorPtr<fuchsia::modular::OngoingActivityType> activities) {
          if (story_id == story_id_ && activities->size() == 1 &&
              activities.get()[0] ==
                  fuchsia::modular::OngoingActivityType::VIDEO) {
            on_watch_ongoing_activities_dispatched.Pass();
          }
          PerformSecondModuleStartActivity();
        });
  }

  TestPoint on_change_all_ongoing_activities_dispatched{
      "When there is a new ongoing activity, all ongoing activities should be "
      "dispatched."};
  // Signals the second module to call ModuleContext.StartOngoingActivity().
  void PerformSecondModuleStartActivity() {
    Signal(kSecondModuleCallStartActivity);
    story_activity_watcher_.OnNotify(
        [this](
            fidl::StringPtr story_id,
            fidl::VectorPtr<fuchsia::modular::OngoingActivityType> activities) {
          if (story_id == story_id_ && activities->size() == 2 &&
              activities.get()[0] ==
                  fuchsia::modular::OngoingActivityType::VIDEO &&
              activities.get()[1] ==
                  fuchsia::modular::OngoingActivityType::VIDEO) {
            on_change_all_ongoing_activities_dispatched.Pass();
          }
          PerformSecondModuleStopActivity();
        });
  }

  TestPoint on_stop_ongoing_activities_dispatched{
      "When an ongoing activity is stopped, all remaining ongoing activities "
      "should be dispatched."};
  // Signals the second module to stop ongoing activity.
  void PerformSecondModuleStopActivity() {
    Signal(kSecondModuleCallStopActivity);
    story_activity_watcher_.OnNotify(
        [this](
            fidl::StringPtr story_id,
            fidl::VectorPtr<fuchsia::modular::OngoingActivityType> activities) {
          if (story_id == story_id_ && activities->size() == 1 &&
              activities.get()[0] ==
                  fuchsia::modular::OngoingActivityType::VIDEO) {
            on_stop_ongoing_activities_dispatched.Pass();
          }
          PerformFirstModuleDone();
        });
  }

  TestPoint on_done_ongoing_activities_stopped{
      "When a module is teared down, the ongoing activity should also be "
      "stopped"};
  TestPoint second_module_active_{
      "Only second module is still active after first calls Done()"};
  // Signals the first module to call ModuleContext.Done().
  void PerformFirstModuleDone() {
    Signal(kFirstModuleCallDone);
    Await(kFirstModuleTerminated, [this] {
      // Verify that the second module is still active, but the first one is
      // not.
      story_controller_->GetActiveModules(
          nullptr,
          [this](fidl::VectorPtr<fuchsia::modular::ModuleData> module_data) {
            if (module_data->size() == 1) {
              second_module_active_.Pass();
            }
            VerifyStoryStillRunning();
          });
    });

    story_activity_watcher_.OnNotify(
        [this](
            fidl::StringPtr story_id,
            fidl::VectorPtr<fuchsia::modular::OngoingActivityType> activities) {
          if (story_id == story_id_ && activities->empty()) {
            on_done_ongoing_activities_stopped.Pass();
          }
        });
  }

  TestPoint story_still_active_{
      "The story is still active after first module calls Done()"};
  // Verifies that the story is still running after the first module has called
  // done and been stopped.
  void VerifyStoryStillRunning() {
    IsStoryRunning([this](bool is_running) {
      if (is_running) {
        story_still_active_.Pass();
      }
      PerformSecondModuleDone();
    });
  }

  TestPoint no_module_active_{
      "No modules are active after second mod calls Done()"};
  TestPoint story_stopped_{"The story was stopped."};
  // Signals the second module to call ModuleContext.Done.
  void PerformSecondModuleDone() {
    Signal(kSecondModuleCallDone);
    Await(kSecondModuleTerminated, [this] {
      // Verify that the second module is still active.
      story_controller_->GetActiveModules(
          nullptr,
          [this](fidl::VectorPtr<fuchsia::modular::ModuleData> module_data) {
            if (module_data->empty()) {
              no_module_active_.Pass();
            }
            IsStoryRunning([this](bool is_running) {
              if (!is_running) {
                story_stopped_.Pass();
              }
              user_shell_context_->Logout();
            });
          });
    });
  }

  // Verifies that the story is stopped when the last module that is part of the
  // story calls ModuleContext.Done and is stopped.
  void IsStoryRunning(std::function<void(bool)> callback) {
    story_provider_->RunningStories(
        [this, callback](fidl::VectorPtr<fidl::StringPtr> story_ids) {
          bool found_story = false;
          // Check all the running stories to make sure the one created for this
          // test is no longer running.
          for (const auto& story_id : *story_ids) {
            found_story |= story_id == story_id_;
          }
          callback(found_story);
        });
  }

  // Creates an intent with one parameter, kLinkName, with the following
  // contents: { |kLinkKey| : |parameter_string| }.
  fuchsia::modular::Intent IntentWithParameterString(
      std::string parameter_string) {
    fuchsia::modular::Intent intent;
    intent.handler = kModulePackageName;

    fuchsia::modular::IntentParameter parameter;
    parameter.name = kLinkName;

    rapidjson::Document document;
    document.SetObject();
    document.AddMember(kLinkKey, parameter_string, document.GetAllocator());

    fuchsia::modular::IntentParameterData parameter_data;
    fsl::SizedVmo vmo;
    FXL_CHECK(fsl::VmoFromString(modular::JsonValueToString(document), &vmo));
    parameter_data.set_json(std::move(vmo).ToTransport());
    parameter.data = std::move(parameter_data);
    intent.parameters.push_back(std::move(parameter));

    return intent;
  }

  fuchsia::modular::UserShellContextPtr user_shell_context_;
  fuchsia::modular::StoryProviderPtr story_provider_;
  fuchsia::modular::StoryControllerPtr story_controller_;
  StoryActivityWatcherImpl story_activity_watcher_;

  std::string story_id_ = "";

  fidl::InterfaceHandle<fuchsia::ui::viewsv1token::ViewOwner> story_view_;

  fxl::WeakPtrFactory<TestApp> weak_ptr_factory_;

  FXL_DISALLOW_COPY_AND_ASSIGN(TestApp);
};

}  // namespace

int main(int /*argc*/, const char** /*argv*/) {
  modular::testing::ComponentMain<TestApp>();
  return 0;
}
