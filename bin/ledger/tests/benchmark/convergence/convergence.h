// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_LEDGER_TESTS_BENCHMARK_CONVERGENCE_CONVERGENCE_H_
#define PERIDOT_BIN_LEDGER_TESTS_BENCHMARK_CONVERGENCE_CONVERGENCE_H_

#include <memory>
#include <set>
#include <vector>

#include <lib/async-loop/cpp/loop.h>
#include <lib/component/cpp/startup_context.h>
#include <lib/fit/function.h>
#include <lib/fxl/files/scoped_temp_dir.h>

#include "peridot/bin/cloud_provider_firestore/testing/cloud_provider_factory.h"
#include "peridot/bin/ledger/fidl/include/types.h"
#include "peridot/bin/ledger/testing/data_generator.h"
#include "peridot/bin/ledger/testing/sync_params.h"

namespace ledger {

// Benchmark that measures the time it takes to sync and reconcile concurrent
// writes.
//
// In this scenario there are specified number of (emulated) devices. At each
// step, every device makes a concurrent write, and we measure the time until
// all the changes are visible to all devices.
//
// Parameters:
//   --entry-count=<int> the number of entries to be put by each device
//   --value-size=<int> the size of a single value in bytes
//   --device-count=<int> number of devices writing to the same page
//   --api-key=<string> the API key used to access the Firestore instance
//   --credentials-path=<file path> Firestore service account credentials
class ConvergenceBenchmark : public PageWatcher {
 public:
  ConvergenceBenchmark(async::Loop* loop, int entry_count, int value_size,
                       int device_count, SyncParams sync_params);

  void Run();

  // PageWatcher:
  void OnChange(PageChange page_change, ResultState result_state,
                OnChangeCallback callback) override;

 private:
  struct DeviceContext;

  void Start(int step);

  void ShutDown();
  fit::closure QuitLoopClosure();

  async::Loop* const loop_;
  DataGenerator generator_;
  std::unique_ptr<component::StartupContext> startup_context_;
  cloud_provider_firestore::CloudProviderFactory cloud_provider_factory_;
  const int entry_count_;
  const int value_size_;
  const int device_count_;
  const std::string user_id_;
  // Track all Ledger instances running for this test and allow to interact with
  // it.
  std::vector<std::unique_ptr<DeviceContext>> devices_;
  PageId page_id_;
  std::multiset<std::string> remaining_keys_;
  int current_step_ = -1;

  FXL_DISALLOW_COPY_AND_ASSIGN(ConvergenceBenchmark);
};

}  // namespace ledger

#endif  // PERIDOT_BIN_LEDGER_TESTS_BENCHMARK_CONVERGENCE_CONVERGENCE_H_
