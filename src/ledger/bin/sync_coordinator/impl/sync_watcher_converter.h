// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LEDGER_BIN_SYNC_COORDINATOR_IMPL_SYNC_WATCHER_CONVERTER_H_
#define SRC_LEDGER_BIN_SYNC_COORDINATOR_IMPL_SYNC_WATCHER_CONVERTER_H_

#include "src/ledger/bin/cloud_sync/public/sync_state_watcher.h"
#include "src/ledger/bin/sync_coordinator/public/sync_state_watcher.h"

namespace sync_coordinator {

// Watcher interface for the current state of data synchronization.
class SyncWatcherConverter : public cloud_sync::SyncStateWatcher {
 public:
  explicit SyncWatcherConverter(sync_coordinator::SyncStateWatcher* watcher);
  ~SyncWatcherConverter() override;

  // Notifies the watcher of a new state.
  void Notify(SyncStateContainer sync_state) override;

 private:
  sync_coordinator::SyncStateWatcher* const watcher_;
};

}  // namespace sync_coordinator

#endif  // SRC_LEDGER_BIN_SYNC_COORDINATOR_IMPL_SYNC_WATCHER_CONVERTER_H_
