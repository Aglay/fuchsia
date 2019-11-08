// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "harvester.h"

#include <lib/async/cpp/task.h>
#include <lib/async/cpp/time.h>
#include <lib/async/dispatcher.h>
#include <lib/zx/time.h>

#include <memory>

#include "gather_cpu.h"
#include "gather_inspectable.h"
#include "gather_introspection.h"
#include "gather_memory.h"
#include "gather_tasks.h"
#include "src/lib/fxl/logging.h"
#include "src/lib/inspect_deprecated/query/discover.h"

namespace harvester {

std::ostream& operator<<(std::ostream& out, const DockyardProxyStatus& status) {
  switch (status) {
    case DockyardProxyStatus::OK:
      return out << "OK (0)";
    case DockyardProxyStatus::ERROR:
      return out << "ERROR (-1)";
  }
  FXL_NOTREACHED();
  return out;
}

Harvester::Harvester(zx_handle_t root_resource,
                     async_dispatcher_t* fast_dispatcher,
                     async_dispatcher_t* slow_dispatcher,
                     std::unique_ptr<DockyardProxy> dockyard_proxy)
    : root_resource_(root_resource),
      fast_dispatcher_(fast_dispatcher),
      slow_dispatcher_(slow_dispatcher),
      dockyard_proxy_(std::move(dockyard_proxy)) {}

void Harvester::GatherDeviceProperties() {
  gather_cpu_.GatherDeviceProperties();
  // TODO(fxb/40872): re-enable once we need this data.
  // gather_inspectable_.GatherDeviceProperties();
  // gather_introspection_.GatherDeviceProperties();
  gather_memory_.GatherDeviceProperties();
  gather_tasks_.GatherDeviceProperties();
}

void Harvester::GatherFastData() {
  zx::time now = async::Now(fast_dispatcher_);

  gather_cpu_.PostUpdate(fast_dispatcher_, now, zx::msec(100));
}

void Harvester::GatherSlowData() {
  zx::time now = async::Now(slow_dispatcher_);

  // TODO(fxb/40872): re-enable once we need this data.
  // gather_inspectable_.PostUpdate(slow_dispatcher_, now, zx::sec(3));
  // gather_introspection_.PostUpdate(slow_dispatcher_, now, zx::sec(10));
  gather_memory_.PostUpdate(slow_dispatcher_, now, zx::msec(100));
  gather_tasks_.PostUpdate(slow_dispatcher_, now, zx::sec(2));
}

}  // namespace harvester
