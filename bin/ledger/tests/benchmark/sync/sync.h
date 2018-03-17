// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_LEDGER_TESTS_BENCHMARK_SYNC_SYNC_H_
#define PERIDOT_BIN_LEDGER_TESTS_BENCHMARK_SYNC_SYNC_H_

#include <memory>

#include "lib/app/cpp/application_context.h"
#include "lib/fxl/files/scoped_temp_dir.h"
#include "lib/ledger/fidl/ledger.fidl.h"
#include "peridot/bin/ledger/testing/cloud_provider_firebase_factory.h"
#include "peridot/bin/ledger/testing/data_generator.h"
#include "peridot/bin/ledger/testing/page_data_generator.h"
#include "peridot/lib/firebase_auth/testing/fake_token_provider.h"

namespace test {
namespace benchmark {

// Benchmark that measures sync latency between two Ledger instances syncing
// through the cloud. This emulates syncing between devices, as the Ledger
// instances have separate disk storage.
//
// Cloud sync needs to be configured on the device in order for the benchmark to
// run.
//
// Parameters:
//   --change-count=<int> the number of changes to be made to the page (each
//   change is done as transaction and can include several put operations).
//   --value-size=<int> the size of a single value in bytes
//   --entries-per-change=<int> number of entries added in the transaction
//   --refs=(on|off) reference strategy: on to put values as references, off to
//     put them as FIDL arrays.
//   --server-id=<string> the ID of the Firebase instance ot use for syncing
class SyncBenchmark : public ledger::PageWatcher {
 public:
  SyncBenchmark(size_t change_count,
                size_t value_size,
                size_t entries_per_change,
                PageDataGenerator::ReferenceStrategy reference_strategy,
                std::string server_id);

  void Run();

  // ledger::PageWatcher:
  void OnChange(ledger::PageChangePtr page_change,
                ledger::ResultState result_state,
                const OnChangeCallback& callback) override;

 private:
  void RunSingleChange(size_t i);

  void ShutDown();

  test::DataGenerator generator_;
  PageDataGenerator page_data_generator_;
  std::unique_ptr<component::ApplicationContext> application_context_;
  test::CloudProviderFirebaseFactory cloud_provider_firebase_factory_;
  const size_t change_count_;
  const size_t value_size_;
  const size_t entries_per_change_;
  const PageDataGenerator::ReferenceStrategy reference_strategy_;
  std::string server_id_;
  f1dl::Binding<ledger::PageWatcher> page_watcher_binding_;
  files::ScopedTempDir alpha_tmp_dir_;
  files::ScopedTempDir beta_tmp_dir_;
  component::ApplicationControllerPtr alpha_controller_;
  component::ApplicationControllerPtr beta_controller_;
  f1dl::Array<uint8_t> page_id_;
  ledger::PagePtr alpha_page_;
  ledger::PagePtr beta_page_;

  size_t changed_entries_received_;

  FXL_DISALLOW_COPY_AND_ASSIGN(SyncBenchmark);
};

}  // namespace benchmark
}  // namespace test

#endif  // PERIDOT_BIN_LEDGER_TESTS_BENCHMARK_SYNC_SYNC_H_
