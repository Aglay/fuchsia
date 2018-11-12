// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>

#include <fuchsia/modular/cpp/fidl.h>
#include <fuchsia/ui/viewsv1token/cpp/fidl.h>
#include <lib/callback/scoped_callback.h>
#include <lib/component/cpp/connect.h>
#include <lib/component/cpp/startup_context.h>
#include <lib/fidl/cpp/binding.h>
#include <lib/fxl/logging.h>
#include <lib/fxl/macros.h>

#include "peridot/lib/testing/component_base.h"
#include "peridot/public/lib/integration_testing/cpp/reporting.h"
#include "peridot/public/lib/integration_testing/cpp/testing.h"
#include "peridot/tests/common/defs.h"
#include "peridot/tests/trigger/defs.h"

using ::modular::testing::Await;
using ::modular::testing::TestPoint;

namespace {

const char kStoryName[] = "story";

class TestApp : public modular::testing::ComponentBase<void> {
 public:
  TestApp(component::StartupContext* const startup_context)
      : ComponentBase(startup_context), weak_ptr_factory_(this) {
    TestInit(__FILE__);

    puppet_master_ =
        startup_context
            ->ConnectToEnvironmentService<fuchsia::modular::PuppetMaster>();
    session_shell_context_ = startup_context->ConnectToEnvironmentService<
        fuchsia::modular::SessionShellContext>();
    session_shell_context_->GetStoryProvider(story_provider_.NewRequest());

    CreateStory();
  }

  ~TestApp() override = default;

 private:
  using TestPoint = modular::testing::TestPoint;

  TestPoint story_create_{"Created story."};

  void CreateStory() {
    fidl::VectorPtr<fuchsia::modular::StoryCommand> commands;
    fuchsia::modular::AddMod add_mod;
    add_mod.mod_name.push_back("root");

    add_mod.intent.action = kModuleAction;
    add_mod.intent.handler = kModuleUrl;

    fuchsia::modular::StoryCommand command;
    command.set_add_mod(std::move(add_mod));
    commands.push_back(std::move(command));

    puppet_master_->ControlStory(kStoryName, story_puppet_master_.NewRequest());
    story_puppet_master_->Enqueue(std::move(commands));
    story_puppet_master_->Execute(
        [this](fuchsia::modular::ExecuteResult result) {
          story_create_.Pass();
          StartStory();
        });
    async::PostDelayedTask(
        async_get_default_dispatcher(),
        callback::MakeScoped(weak_ptr_factory_.GetWeakPtr(),
                             [this] { session_shell_context_->Logout(); }),
        zx::msec(kTimeoutMilliseconds));
  }

  TestPoint got_queue_token_{"Got message queue token."};
  TestPoint module_finished_{"Trigger test module finished work."};
  TestPoint story_was_deleted_{"Story was deleted."};
  TestPoint agent_executed_delete_task_{
      "fuchsia::modular::Agent executed message queue task."};
  void StartStory() {
    story_provider_->GetController(kStoryName, story_controller_.NewRequest());
    story_controller_.set_error_handler([this](zx_status_t status) {
      FXL_LOG(ERROR) << "Story controller for story " << kStoryName
                     << " died. Does this story exist?";
    });

    story_controller_->Start(story_view_.NewRequest());

    // Retrieve the message queue token for the messsage queue that the module
    // created.
    modular::testing::GetStore()->Get(
        "trigger_test_module_queue_token", [this](fidl::StringPtr value) {
          got_queue_token_.Pass();
          // Wait for the module to finish its test cases for communicating
          // with the agent.
          Await("trigger_test_module_done", [this, value] {
            module_finished_.Pass();
            // Delete the story to trigger the deletion of the message
            // queue that the module created.
            puppet_master_->DeleteStory(kStoryName, [this, value]() {
              story_was_deleted_.Pass();
              // Verify that the agent task was triggered, by checking
              // that the agent wrote the message queue token to the
              // test store.
              Await(value, [this] {
                agent_executed_delete_task_.Pass();
                session_shell_context_->Logout();
              });
            });
          });
        });
  }

  fuchsia::modular::PuppetMasterPtr puppet_master_;
  fuchsia::modular::StoryPuppetMasterPtr story_puppet_master_;
  fuchsia::modular::SessionShellContextPtr session_shell_context_;
  fuchsia::modular::StoryProviderPtr story_provider_;
  fuchsia::modular::StoryControllerPtr story_controller_;

  fidl::InterfaceHandle<fuchsia::ui::viewsv1token::ViewOwner> story_view_;

  fxl::WeakPtrFactory<TestApp> weak_ptr_factory_;

  FXL_DISALLOW_COPY_AND_ASSIGN(TestApp);
};

}  // namespace

int main(int /*argc*/, const char** /*argv*/) {
  modular::testing::ComponentMain<TestApp>();
  return 0;
}
