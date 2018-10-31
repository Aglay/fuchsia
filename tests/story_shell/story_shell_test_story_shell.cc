// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Implementation of the fuchsia::modular::StoryShell service that just lays out
// the views of all modules side by side.

#include <memory>

#include <fuchsia/modular/cpp/fidl.h>
#include <fuchsia/ui/policy/cpp/fidl.h>
#include <fuchsia/ui/viewsv1token/cpp/fidl.h>
#include <lib/app_driver/cpp/app_driver.h>
#include <lib/component/cpp/startup_context.h>
#include <lib/fsl/vmo/strings.h>
#include <lib/fxl/command_line.h>
#include <lib/fxl/logging.h>
#include <lib/fxl/macros.h>

#include "peridot/lib/rapidjson/rapidjson.h"
#include "peridot/lib/testing/component_base.h"
#include "peridot/public/lib/integration_testing/cpp/reporting.h"
#include "peridot/public/lib/integration_testing/cpp/testing.h"
#include "peridot/tests/common/defs.h"
#include "peridot/tests/story_shell/defs.h"
#include "rapidjson/document.h"

using modular::testing::Signal;

namespace {

// Cf. README.md for what this test does and how.
class TestApp
    : public modular::testing::ComponentBase<fuchsia::modular::StoryShell> {
 public:
  TestApp(component::StartupContext* const startup_context)
      : ComponentBase(startup_context) {
    TestInit(__FILE__);
  }

  ~TestApp() override = default;

 private:
  // |fuchsia::modular::StoryShell|
  void Initialize(fidl::InterfaceHandle<fuchsia::modular::StoryShellContext>
                      story_shell_context) override {
    story_shell_context_.Bind(std::move(story_shell_context));
    story_shell_context_->GetPresentation(presentation_.NewRequest());
    story_shell_context_->GetLink(story_shell_link_.NewRequest());
    fidl::VectorPtr<fidl::StringPtr> link_path;
    link_path.push_back("path");

    story_shell_link_->Get(
        link_path.Clone(),
        [this](std::unique_ptr<fuchsia::mem::Buffer> content) {
          std::string data_string;
          fsl::StringFromVmo(*content, &data_string);
          Signal("story link data: " + data_string);

          rapidjson::Document document;
          document.SetObject();
          document.AddMember("label", "value", document.GetAllocator());
          fsl::SizedVmo vmo;
          fidl::VectorPtr<fidl::StringPtr> path_to_write;
          path_to_write.push_back("path");
          fsl::VmoFromString(modular::JsonValueToString(document), &vmo);
          fuchsia::mem::Buffer buffer = std::move(vmo).ToTransport();
          story_shell_link_->Set(path_to_write.Clone(), std::move(buffer));
        });
  }

  // Keep state to check ordering. Cf. below.
  bool seen_root_one_{};

  // |fuchsia::modular::StoryShell|
  void AddView(
      fidl::InterfaceHandle<fuchsia::ui::viewsv1token::ViewOwner> view_owner,
      fidl::StringPtr view_id, fidl::StringPtr anchor_id,
      fuchsia::modular::SurfaceRelationPtr /*surface_relation*/,
      fuchsia::modular::ModuleManifestPtr module_manifest,
      fuchsia::modular::ModuleSource /* module_source */) override {
    FXL_LOG(INFO) << "AddView " << view_id << " " << anchor_id << " "
                  << (module_manifest ? module_manifest->composition_pattern
                                      : " NO MANIFEST");
    if (view_id == "root:one" && anchor_id == "root") {
      Signal("root:one");

      if (module_manifest && module_manifest->composition_pattern == "ticker" &&
          module_manifest->intent_filters->size() == 1 &&
          module_manifest->intent_filters.get()[0].action ==
              kCommonNullAction) {
        Signal("root:one manifest");
      }

      seen_root_one_ = true;
    }

    if (view_id == "root:one:two" && anchor_id == "root:one") {
      Signal("root:one:two");

      if (module_manifest && module_manifest->composition_pattern == "ticker" &&
          module_manifest->intent_filters->size() == 1 &&
          module_manifest->intent_filters.get()[0].action ==
              kCommonNullAction) {
        Signal("root:one:two manifest");
      }

      if (seen_root_one_) {
        Signal("root:one:two ordering");
      }
    }
  }

  // |fuchsia::modular::StoryShell|
  void FocusView(fidl::StringPtr /*view_id*/,
                 fidl::StringPtr /*relative_view_id*/) override {}

  // |fuchsia::modular::StoryShell|
  void DefocusView(fidl::StringPtr /*view_id*/,
                   DefocusViewCallback callback) override {
    callback();
  }

  // |fuchsia::modular::StoryShell|
  void AddContainer(
      fidl::StringPtr /*container_name*/, fidl::StringPtr /*parent_id*/,
      fuchsia::modular::SurfaceRelation /*relation*/,
      fidl::VectorPtr<fuchsia::modular::ContainerLayout> /*layout*/,
      fidl::VectorPtr<
          fuchsia::modular::ContainerRelationEntry> /* relationships */,
      fidl::VectorPtr<fuchsia::modular::ContainerView> /* views */) override {}

  fuchsia::modular::StoryShellContextPtr story_shell_context_;
  fuchsia::modular::LinkPtr story_shell_link_;
  fuchsia::ui::policy::PresentationPtr presentation_;

  FXL_DISALLOW_COPY_AND_ASSIGN(TestApp);
};

}  // namespace

int main(int /* argc */, const char** /* argv */) {
  FXL_LOG(INFO) << "Story Shell main";
  modular::testing::ComponentMain<TestApp>();
  return 0;
}
