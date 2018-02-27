// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_LEDGER_TESTS_INTEGRATION_SYNC_LIB_H_
#define PERIDOT_BIN_LEDGER_TESTS_INTEGRATION_SYNC_LIB_H_

#include <functional>
#include <memory>

#include <trace-provider/provider.h>

#include "garnet/lib/gtest/test_with_message_loop.h"
#include "gtest/gtest.h"
#include "lib/app/cpp/application_context.h"
#include "lib/fxl/files/scoped_temp_dir.h"
#include "lib/fxl/macros.h"
#include "lib/ledger/fidl/ledger.fidl.h"
#include "peridot/bin/ledger/fidl_helpers/bound_interface_set.h"
#include "peridot/bin/ledger/testing/ledger_app_instance_factory.h"
#include "peridot/lib/firebase_auth/testing/fake_token_provider.h"

namespace test {
namespace integration {
namespace sync {

// Base test class for synchronization tests. Other tests should derive from
// this class to use the proper synchronization configuration.
class SyncTest : public gtest::TestWithMessageLoop {
 public:
  SyncTest();
  ~SyncTest() override;

  std::unique_ptr<LedgerAppInstanceFactory::LedgerAppInstance>
  NewLedgerAppInstance();

 protected:
  void SetUp() override;

 private:
  std::unique_ptr<trace::TraceProvider> trace_provider_;
  std::unique_ptr<LedgerAppInstanceFactory> app_factory_;
};

// Initializes test environment based on the command line arguments.
//
// Returns true iff the initialization was successful.
bool ProcessCommandLine(int argc, char** argv);

}  // namespace sync
}  // namespace integration
}  // namespace test

#endif  // PERIDOT_BIN_LEDGER_TESTS_INTEGRATION_SYNC_LIB_H_
