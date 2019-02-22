// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/modular/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/component/cpp/startup_context.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/fxl/command_line.h>
#include <lib/fxl/log_settings_command_line.h>
#include <trace-provider/provider.h>

#include "peridot/lib/rng/system_random.h"
#include "src/ledger/cloud_provider_firestore/bin/app/factory_impl.h"

namespace cloud_provider_firestore {
namespace {

constexpr fxl::StringView kCobaltClientName = "cloud_provider_firestore";
constexpr fxl::StringView kNoStatisticsReporting = "disable_reporting";

struct AppParams {
  bool disable_statistics = false;
};

class App : public fuchsia::modular::Lifecycle {
 public:
  explicit App(AppParams app_params)
      : loop_(&kAsyncLoopConfigAttachToThread),
        startup_context_(component::StartupContext::CreateFromStartupInfo()),
        trace_provider_(loop_.dispatcher()),
        factory_impl_(
            loop_.dispatcher(), &random_, startup_context_.get(),
            app_params.disable_statistics ? "" : kCobaltClientName.ToString()) {
    FXL_DCHECK(startup_context_);
  }

  void Run() {
    startup_context_->outgoing().AddPublicService<fuchsia::modular::Lifecycle>(
        [this](fidl::InterfaceRequest<fuchsia::modular::Lifecycle> request) {
          lifecycle_bindings_.AddBinding(this, std::move(request));
        });
    startup_context_->outgoing().AddPublicService<Factory>(
        [this](fidl::InterfaceRequest<Factory> request) {
          factory_bindings_.AddBinding(&factory_impl_, std::move(request));
        });
    loop_.Run();
  }

  void Terminate() override {
    factory_impl_.ShutDown([this] { loop_.Quit(); });
  }

 private:
  async::Loop loop_;
  rng::SystemRandom random_;
  std::unique_ptr<component::StartupContext> startup_context_;
  trace::TraceProvider trace_provider_;

  FactoryImpl factory_impl_;
  fidl::BindingSet<fuchsia::modular::Lifecycle> lifecycle_bindings_;
  fidl::BindingSet<Factory> factory_bindings_;

  FXL_DISALLOW_COPY_AND_ASSIGN(App);
};
}  // namespace

}  // namespace cloud_provider_firestore

int main(int argc, const char** argv) {
  // The trust root file is made available by the sandbox feature
  // "root-ssl-certificates"
  setenv("GRPC_DEFAULT_SSL_ROOTS_FILE_PATH", "/config/ssl/cert.pem", 1);

  const auto command_line = fxl::CommandLineFromArgcArgv(argc, argv);
  fxl::SetLogSettingsFromCommandLine(command_line);

  cloud_provider_firestore::AppParams app_params;
  app_params.disable_statistics =
      command_line.HasOption(cloud_provider_firestore::kNoStatisticsReporting);

  cloud_provider_firestore::App app(app_params);
  app.Run();

  return 0;
}
