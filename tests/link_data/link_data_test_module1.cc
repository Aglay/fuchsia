// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/cpp/modular.h>
#include <fuchsia/cpp/views_v1.h>
#include "lib/app_driver/cpp/module_driver.h"
#include "lib/fsl/tasks/message_loop.h"
#include "peridot/lib/rapidjson/rapidjson.h"
#include "peridot/lib/testing/reporting.h"
#include "peridot/lib/testing/testing.h"
#include "peridot/tests/common/defs.h"
#include "peridot/tests/link_data/defs.h"

namespace {

// Cf. README.md for what this test does and how.
class TestApp {
 public:
  TestApp(
      modular::ModuleHost* const module_host,
      fidl::InterfaceRequest<views_v1::ViewProvider> /*view_provider_request*/,
      fidl::InterfaceRequest<
      component::ServiceProvider> /*outgoing_services*/)
      : module_host_(module_host) {
    modular::testing::Init(module_host->application_context(), __FILE__);
    modular::testing::GetStore()->Put("module1_init", "", [] {});
    module_host_->module_context()->Ready();

    path_.push_back(kCount);

    Start();
  }

  void Start() {
    module_host_->module_context()->GetLink("link", link_.NewRequest());
    Loop();
  }

  void Loop() {
    link_->Get(path_.Clone(), [this](fidl::StringPtr value) {
        if (!value.is_null()) {
          modular::testing::GetStore()->Put("module1_link", value, [] {});
        }
        rapidjson::Document doc;
        doc.Parse(value.get().c_str());
        if (doc.IsInt()) {
          doc.SetInt(doc.GetInt() + 1);
        } else {
          doc.SetInt(0);
        }
        link_->Set(path_.Clone(), modular::JsonValueToString(doc));
        link_->Sync([this] { Loop(); });
      });
  }

  // Called from ModuleDriver.
  void Terminate(const std::function<void()>& done) {
    modular::testing::GetStore()->Put("module1_stop", "", [] {});
    modular::testing::Done(done);
  }

 private:
  modular::ModuleHost* const module_host_;
  modular::LinkPtr link_;

  fidl::VectorPtr<fidl::StringPtr> path_;

  FXL_DISALLOW_COPY_AND_ASSIGN(TestApp);
};

}  // namespace

int main(int /*argc*/, const char** /*argv*/) {
  fsl::MessageLoop loop;
  auto app_context = component::ApplicationContext::CreateFromStartupInfo();
  modular::ModuleDriver<TestApp> driver(app_context.get(),
                                         [&loop] { loop.QuitNow(); });
  loop.Run();
  return 0;
}
