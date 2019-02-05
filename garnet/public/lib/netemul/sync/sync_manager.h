// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_NETEMUL_SYNC_SYNC_MANAGER_H_
#define LIB_NETEMUL_SYNC_SYNC_MANAGER_H_

#include <fuchsia/netemul/sync/cpp/fidl.h>
#include <lib/async/default.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/fxl/macros.h>
#include <unordered_map>
#include "bus.h"

namespace netemul {

class SyncManager : public fuchsia::netemul::sync::SyncManager {
 public:
  using FSyncManager = fuchsia::netemul::sync::SyncManager;

  SyncManager() : SyncManager(async_get_default_dispatcher()) {}

  explicit SyncManager(async_dispatcher_t* dispatcher)
      : dispatcher_(dispatcher) {}

  void BusSubscribe(std::string busName, std::string clientName,
                    ::fidl::InterfaceRequest<Bus::FBus> bus) override;

  fidl::InterfaceRequestHandler<FSyncManager> GetHandler();

 private:
  Bus& GetBus(const std::string& name);

  async_dispatcher_t* dispatcher_;
  std::unordered_map<std::string, Bus::Ptr> buses_;
  fidl::BindingSet<FSyncManager> bindings_;

  FXL_DISALLOW_COPY_AND_ASSIGN(SyncManager);
};

}  // namespace netemul

#endif  // LIB_NETEMUL_SYNC_SYNC_MANAGER_H_
