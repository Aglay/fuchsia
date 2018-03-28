// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Implementation of the StoryShell service that just lays out the
// views of all modules side by side.

#include <memory>

#include <fuchsia/cpp/modular.h>
#include <fuchsia/cpp/views_v1_token.h>
#include "lib/app/cpp/application_context.h"
#include "lib/app_driver/cpp/app_driver.h"
#include "lib/fsl/tasks/message_loop.h"
#include "lib/fxl/command_line.h"
#include "lib/fxl/logging.h"
#include "lib/fxl/macros.h"
#include "peridot/lib/testing/component_base.h"
#include "peridot/lib/testing/reporting.h"
#include "peridot/lib/testing/testing.h"

namespace {

class TestApp : public modular::testing::ComponentBase<modular::StoryShell> {
 public:
  TestApp(component::ApplicationContext* const application_context)
      : ComponentBase(application_context) {
    TestInit(__FILE__);
  }

  ~TestApp() override = default;

 private:
  using TestPoint = modular::testing::TestPoint;

  // |StoryShell|
  void Initialize(
      fidl::InterfaceHandle<modular::StoryContext> story_context) override {
    story_context_.Bind(std::move(story_context));
  }

  TestPoint connect_view_{"ConnectView root:child:child root"};

  // |StoryShell|
  void ConnectView(fidl::InterfaceHandle<views_v1_token::ViewOwner> view_owner,
                   fidl::StringPtr view_id,
                   fidl::StringPtr anchor_id,
                   modular::SurfaceRelationPtr /*surface_relation*/,
                   modular::ModuleManifestPtr /*module_manifest*/) override {
    if (view_id == "root:child:child" && anchor_id == "root") {
      connect_view_.Pass();
      modular::testing::GetStore()->Put("story_shell_done", "1", [] {});
    } else {
      FXL_LOG(WARNING) << "ConnectView " << view_id << " anchor " << anchor_id;
    }
  }

  // |StoryShell|
  void FocusView(fidl::StringPtr /*view_id*/,
                 fidl::StringPtr /*relative_view_id*/) override {}

  // |StoryShell|
  void DefocusView(fidl::StringPtr /*view_id*/,
                   DefocusViewCallback callback) override {
    callback();
  }

  // |StoryShell|
  void AddContainer(
      fidl::StringPtr /*container_name*/,
      fidl::StringPtr /*parent_id*/,
      modular::SurfaceRelation /*relation*/,
      fidl::VectorPtr<modular::ContainerLayout> /*layout*/,
      fidl::VectorPtr<modular::ContainerRelationEntry> /* relationships */,
      fidl::VectorPtr<modular::ContainerView> /* views */) override {}

  modular::StoryContextPtr story_context_;
  FXL_DISALLOW_COPY_AND_ASSIGN(TestApp);
};

}  // namespace

int main(int /* argc */, const char** /* argv */) {
  FXL_LOG(INFO) << "Embed Story Shell main";
  modular::testing::ComponentMain<TestApp>();
  return 0;
}
