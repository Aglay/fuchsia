// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <utility>

#include <fuchsia/modular/internal/cpp/fidl.h>
#include <fuchsia/ui/viewsv1token/cpp/fidl.h>
#include <lib/async/cpp/task.h>
#include <lib/async/default.h>
#include <lib/component/cpp/startup_context.h>
#include <lib/fidl/cpp/binding.h>
#include <lib/fsl/vmo/strings.h>
#include <lib/fxl/command_line.h>
#include <lib/fxl/logging.h>
#include <lib/fxl/macros.h>

#include "peridot/lib/common/names.h"
#include "peridot/lib/rapidjson/rapidjson.h"
#include "peridot/lib/testing/component_main.h"
#include "peridot/lib/testing/session_shell_base.h"
#include "peridot/public/lib/integration_testing/cpp/reporting.h"
#include "peridot/public/lib/integration_testing/cpp/testing.h"
#include "peridot/tests/common/defs.h"
#include "peridot/tests/link_data/defs.h"

using modular::testing::Await;
using modular::testing::Get;
using modular::testing::Signal;
using modular::testing::TestPoint;

namespace {

const char kStoryName[] = "story";

// Cf. README.md for what this test does and how.
class TestApp : public modular::testing::SessionShellBase {
 public:
  explicit TestApp(component::StartupContext* const startup_context)
      : SessionShellBase(startup_context) {
    TestInit(__FILE__);

    startup_context->ConnectToEnvironmentService(puppet_master_.NewRequest());

    TestStory1();
  }

  ~TestApp() override = default;

 private:
  TestPoint story1_create_{"Story1 Create"};

  void TestStory1() {
    std::vector<fuchsia::modular::StoryCommand> commands;
    fuchsia::modular::AddMod add_mod;
    add_mod.mod_name.push_back(kModule0Name);
    add_mod.intent.action = kModule0Action;
    add_mod.intent.handler = kModule0Url;

    fuchsia::modular::IntentParameterData data;
    fsl::SizedVmo vmo;
    FXL_CHECK(fsl::VmoFromString(kRootJson0, &vmo));
    data.set_json(std::move(vmo).ToTransport());
    fuchsia::modular::IntentParameter intent_parameter;
    intent_parameter.name = kModule0Link;
    intent_parameter.data = std::move(data);
    add_mod.intent.parameters.push_back(std::move(intent_parameter));

    fuchsia::modular::StoryCommand command;
    command.set_add_mod(std::move(add_mod));
    commands.push_back(std::move(command));

    puppet_master_->ControlStory(kStoryName, story_puppet_master_.NewRequest());
    story_puppet_master_->Enqueue(std::move(commands));
    story_puppet_master_->Execute(
        [this](fuchsia::modular::ExecuteResult result) {
          story1_create_.Pass();
          TestStory1_GetController();
        });
  }

  TestPoint story1_get_controller_{"Story1 GetController"};

  void TestStory1_GetController() {
    story_provider()->GetController(kStoryName, story_controller_.NewRequest());
    story_controller_->GetInfo([this](fuchsia::modular::StoryInfo story_info,
                                      fuchsia::modular::StoryState state) {
      story1_get_controller_.Pass();
      story_info_ = std::move(story_info);
      TestStory1_GetModule0Link();
    });
  }

  TestPoint story1_get_module0_link_{"Story1 Get Module0 link"};

  void TestStory1_GetModule0Link() {
    fidl::VectorPtr<std::string> module_path;
    module_path.push_back(kModule0Name);
    fuchsia::modular::LinkPath link_path = fuchsia::modular::LinkPath();
    link_path.module_path = std::move(module_path);
    link_path.link_name = kModule0Link;
    story_controller_->GetLink(std::move(link_path),
                               module0_link_.NewRequest());
    module0_link_->Get(nullptr,
                       [this](std::unique_ptr<fuchsia::mem::Buffer> value) {
                         std::string json_string;
                         FXL_CHECK(fsl::StringFromVmo(*value, &json_string));
                         if (json_string == kRootJson0) {
                           story1_get_module0_link_.Pass();
                         } else {
                           FXL_LOG(ERROR) << "GOT LINK " << json_string
                                          << " EXPECTED " << kRootJson0;
                         }
                         TestStory1_SetModule0Link();
                       });
  }

  TestPoint story1_set_module0_link_{"Story1 Set Module0 link"};

  void TestStory1_SetModule0Link() {
    fsl::SizedVmo data;
    auto result = fsl::VmoFromString(kRootJson1, &data);
    FXL_CHECK(result);

    module0_link_->Set(nullptr, std::move(data).ToTransport());
    module0_link_->Get(nullptr,
                       [this](std::unique_ptr<fuchsia::mem::Buffer> value) {
                         std::string json_string;
                         FXL_CHECK(fsl::StringFromVmo(*value, &json_string));
                         if (json_string == kRootJson1) {
                           story1_set_module0_link_.Pass();
                         } else {
                           FXL_LOG(ERROR) << "GOT LINK " << json_string
                                          << " EXPECTED " << kRootJson1;
                         }
                         TestStory1_Run();
                       });
  }

  TestPoint story1_run_module0_link_{"Story1 Run: Module0 link"};

  void TestStory1_Run() {
    story_controller_->RequestStart();

    Await(std::string("module0_link") + ":" + kRootJson1, [this] {
      story1_run_module0_link_.Pass();
      TestStory1_Wait();
    });
  }

  void TestStory1_Wait() {
    Get("module2_link", [this](fidl::StringPtr value) {
      FXL_LOG(INFO) << "GET module2_link " << value;
      rapidjson::Document doc;
      doc.Parse(value);
      if (!doc.IsObject() || !doc.HasMember(kCount) || !doc[kCount].IsInt() ||
          doc[kCount].GetInt() < 10) {
        TestStory1_Wait();
        return;
      }

      TestStory1_Stop();
    });
  }

  TestPoint story1_stop_{"Story1 Stop"};

  void TestStory1_Stop() {
    story_controller_->Stop([this] {
      story1_stop_.Pass();
      TestStory1_GetActiveModules();
    });
  }

  TestPoint story1_get_active_modules_{"Story1 GetActiveModules()"};

  void TestStory1_GetActiveModules() {
    story_controller_->GetActiveModules(
        [this](std::vector<fuchsia::modular::ModuleData> modules) {
          if (modules.size() == 0) {
            story1_get_active_modules_.Pass();
          } else {
            FXL_LOG(ERROR) << "ACTIVE MODULES " << modules.size()
                           << " EXPECTED " << 0;
          }
          TestStory1_GetActiveLinks();
        });
  }

  TestPoint story1_get_active_links_{"Story1 GetActiveLinks()"};

  void TestStory1_GetActiveLinks() {
    story_controller_->GetActiveLinks(
        nullptr, [this](std::vector<fuchsia::modular::LinkPath> links) {
          if (links.size() == 0) {
            story1_get_active_links_.Pass();
          } else {
            FXL_LOG(ERROR) << "ACTIVE LINKS " << links.size() << " EXPECTED "
                           << 0;
          }
          TestStory2_Run();
        });
  }

  TestPoint story2_run_{"Story2 Run"};

  void TestStory2_Run() {
    story2_run_.Pass();

    story_controller_->RequestStart();

    TestStory2_Wait();
  }

  void TestStory2_Wait() {
    Get("module2_link", [this](fidl::StringPtr value) {
      FXL_LOG(INFO) << "GET module2_link " << value;
      rapidjson::Document doc;
      doc.Parse(value);
      if (!doc.IsObject() || !doc.HasMember(kCount) || !doc[kCount].IsInt() ||
          doc[kCount].GetInt() < 20) {
        TestStory2_Wait();
        return;
      }

      TestStory2_Stop();
    });
  }

  TestPoint story2_stop_{"Story2 Stop"};

  void TestStory2_Stop() {
    story_controller_->Stop([this] {
      story2_stop_.Pass();
      TestStory2_Delete();
    });
  }

  TestPoint story2_delete_{"Story2 Delete"};

  void TestStory2_Delete() {
    puppet_master_->DeleteStory(story_info_.id, [this] {
      story2_delete_.Pass();
      Signal(modular::testing::kTestShutdown);
    });
  }

  fuchsia::modular::PuppetMasterPtr puppet_master_;
  fuchsia::modular::StoryPuppetMasterPtr story_puppet_master_;
  fuchsia::modular::StoryControllerPtr story_controller_;
  fuchsia::modular::LinkPtr module0_link_;
  fuchsia::modular::StoryInfo story_info_;

  FXL_DISALLOW_COPY_AND_ASSIGN(TestApp);
};

}  // namespace

int main(int argc, const char** argv) {
  modular::testing::ComponentMain<TestApp>();
  return 0;
}
