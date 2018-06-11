// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/modular/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <trace-provider/provider.h>

#include "lib/app/cpp/startup_context.h"
#include "lib/fidl/cpp/binding_set.h"
#include "lib/fxl/command_line.h"
#include "lib/fxl/log_settings_command_line.h"
#include "peridot/bin/cloud_provider_firestore/app/factory_impl.h"

namespace cloud_provider_firestore {
namespace {

class App : public fuchsia::modular::Lifecycle {
 public:
  App()
      : loop_(&kAsyncLoopConfigMakeDefault),
        startup_context_(fuchsia::sys::StartupContext::CreateFromStartupInfo()),
        trace_provider_(loop_.async()),
        factory_impl_(loop_.async()) {
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
  std::unique_ptr<fuchsia::sys::StartupContext> startup_context_;
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

  cloud_provider_firestore::App app;
  app.Run();

  return 0;
}
