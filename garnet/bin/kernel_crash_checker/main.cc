// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fuchsia/crash/cpp/fidl.h>
#include <fuchsia/net/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/component/cpp/startup_context.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/fdio.h>
#include <lib/fidl/cpp/synchronous_interface_ptr.h>
#include <lib/fsl/vmo/file.h>
#include <lib/fsl/vmo/strings.h>
#include <lib/fxl/logging.h>
#include <lib/syslog/cpp/logger.h>
#include <lib/zx/vmo.h>
#include <zircon/errors.h>
#include <zircon/status.h>
#include <zircon/syscalls.h>
#include <zircon/types.h>

#include "src/lib/files/file.h"
#include "src/lib/files/unique_fd.h"

class CrashAnalyzer {
 public:
  explicit CrashAnalyzer()
      : context_(component::StartupContext::CreateFromStartupInfo()) {
    FXL_DCHECK(context_);
  }

  void ProcessCrashlog(fuchsia::mem::Buffer crashlog) {
    fuchsia::crash::AnalyzerSyncPtr analyzer;
    context_->ConnectToEnvironmentService(analyzer.NewRequest());
    FXL_DCHECK(analyzer);

    zx_status_t out_status;
    const zx_status_t status =
        analyzer->ProcessKernelPanicCrashlog(std::move(crashlog), &out_status);
    if (status != ZX_OK) {
      FX_LOGS(ERROR) << "failed to connect to crash analyzer: " << status
                     << " (" << zx_status_get_string(status) << ")";
    } else if (out_status != ZX_OK) {
      FX_LOGS(ERROR) << "failed to process kernel panic crash log: "
                     << out_status << " (" << zx_status_get_string(out_status)
                     << ")";
    }
  }

 private:
  std::unique_ptr<component::StartupContext> context_;
};

int main(int argc, char** argv) {
  syslog::InitLogger({"crash"});

  const char filepath[] = "/boot/log/last-panic.txt";
  fxl::UniqueFD fd(open(filepath, O_RDONLY));
  if (!fd.is_valid()) {
    FX_LOGS(INFO) << "no kernel crash log found";
    return 0;
  }

  fsl::SizedVmo crashlog_vmo;
  if (!fsl::VmoFromFd(std::move(fd), &crashlog_vmo)) {
    FX_LOGS(ERROR) << "error loading kernel crash log into VMO";
    return 1;
  }

  std::string crashlog_str;
  if (!fsl::StringFromVmo(crashlog_vmo, &crashlog_str)) {
    FX_LOGS(ERROR) << "error converting kernel crash log VMO to string";
    return 1;
  }
  FX_LOGS(INFO) << "dumping log from previous kernel panic:\n" << crashlog_str;

  async::Loop loop(&kAsyncLoopConfigAttachToThread);
  fuchsia::net::ConnectivityPtr connectivity =
      component::StartupContext::CreateFromStartupInfo()
          ->ConnectToEnvironmentService<fuchsia::net::Connectivity>();
  connectivity.events().OnNetworkReachable = [&crashlog_vmo](bool reachable) {
    if (!reachable) {
      return;
    }
    CrashAnalyzer crash_analyzer;
    crash_analyzer.ProcessCrashlog(std::move(crashlog_vmo).ToTransport());
  };
  loop.Run();

  return 0;
}
