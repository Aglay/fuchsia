// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_SYS_APPMGR_APPMGR_H_
#define SRC_SYS_APPMGR_APPMGR_H_

#include <lib/inspect/cpp/inspector.h>
#include <lib/inspect/service/cpp/service.h>
#include <lib/sys/cpp/service_directory.h>

#include <fs/pseudo_dir.h>
#include <fs/remote_dir.h>
#include <fs/service.h>
#include <fs/synchronous_vfs.h>

#include "garnet/lib/loader/package_loader.h"
#include "src/lib/fxl/macros.h"
#include "src/sys/appmgr/cpu_watcher.h"
#include "src/sys/appmgr/realm.h"
#include "src/sys/appmgr/storage_watchdog.h"
#include "src/sys/appmgr/util.h"

namespace component {

struct AppmgrArgs {
  zx_handle_t pa_directory_request;
  fuchsia::sys::ServiceListPtr root_realm_services;
  const std::shared_ptr<sys::ServiceDirectory> environment_services;
  std::string sysmgr_url;
  fidl::VectorPtr<std::string> sysmgr_args;
  bool run_virtual_console;
  bool retry_sysmgr_crash;
  zx::channel trace_server_channel;
};

class Appmgr {
 public:
  Appmgr(async_dispatcher_t* dispatcher, AppmgrArgs args);
  ~Appmgr();

 private:
  void MeasureCpu(async_dispatcher_t* dispatcher);

  inspect::Inspector inspector_;
  std::unique_ptr<CpuWatcher> cpu_watcher_;
  std::unique_ptr<Realm> root_realm_;
  fs::SynchronousVfs publish_vfs_;
  fbl::RefPtr<fs::PseudoDir> publish_dir_;

  fuchsia::sys::ComponentControllerPtr sysmgr_;
  std::string sysmgr_url_;
  fidl::VectorPtr<std::string> sysmgr_args_;
  RestartBackOff sysmgr_backoff_;
  bool sysmgr_retry_crashes_;
  bool sysmgr_permanently_failed_;
  StorageWatchdog storage_watchdog_;

  FXL_DISALLOW_COPY_AND_ASSIGN(Appmgr);
};

}  // namespace component

#endif  // SRC_SYS_APPMGR_APPMGR_H_
