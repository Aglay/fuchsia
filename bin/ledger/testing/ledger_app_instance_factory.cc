// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/ledger/testing/ledger_app_instance_factory.h"

#include <zx/time.h>

#include "gtest/gtest.h"
#include "lib/fxl/functional/make_copyable.h"
#include "lib/fxl/memory/ref_ptr.h"
#include "lib/fxl/tasks/task_runner.h"
#include "peridot/lib/callback/synchronous_task.h"
#include "peridot/lib/convert/convert.h"

namespace test {
namespace {
constexpr zx::duration kTimeout = zx::sec(20);
}

LedgerAppInstanceFactory::LedgerAppInstance::LedgerAppInstance(
    f1dl::Array<uint8_t> test_ledger_name,
    ledger::LedgerRepositoryFactoryPtr ledger_repository_factory)
    : test_ledger_name_(std::move(test_ledger_name)),
      ledger_repository_factory_(std::move(ledger_repository_factory)) {}

LedgerAppInstanceFactory::LedgerAppInstance::~LedgerAppInstance() {}

ledger::LedgerRepositoryFactory*
LedgerAppInstanceFactory::LedgerAppInstance::ledger_repository_factory() {
  return ledger_repository_factory_.get();
}

ledger::LedgerRepositoryPtr
LedgerAppInstanceFactory::LedgerAppInstance::GetTestLedgerRepository() {
  ledger::LedgerRepositoryPtr repository;
  ledger::Status status;
  ledger_repository_factory_->GetRepository(
      dir_.path(), MakeCloudProvider(), repository.NewRequest(),
      [&status](ledger::Status s) { status = s; });
  EXPECT_TRUE(ledger_repository_factory_.WaitForResponseUntil(
      zx::deadline_after(kTimeout)));
  EXPECT_EQ(ledger::Status::OK, status);
  return repository;
}

ledger::LedgerPtr LedgerAppInstanceFactory::LedgerAppInstance::GetTestLedger() {
  ledger::LedgerPtr ledger;

  ledger::LedgerRepositoryPtr repository = GetTestLedgerRepository();
  ledger::Status status;
  repository->GetLedger(test_ledger_name_.Clone(), ledger.NewRequest(),
                        [&status](ledger::Status s) { status = s; });
  EXPECT_TRUE(repository.WaitForResponseUntil(zx::deadline_after(kTimeout)));
  EXPECT_EQ(ledger::Status::OK, status);
  return ledger;
}

ledger::PagePtr LedgerAppInstanceFactory::LedgerAppInstance::GetTestPage() {
  f1dl::InterfaceHandle<ledger::Page> page;
  ledger::Status status;
  ledger::LedgerPtr ledger = GetTestLedger();
  ledger->GetPage(nullptr, page.NewRequest(),
                  [&status](ledger::Status s) { status = s; });
  EXPECT_TRUE(ledger.WaitForResponseUntil(zx::deadline_after(kTimeout)));
  EXPECT_EQ(ledger::Status::OK, status);

  return page.Bind();
}

ledger::PagePtr LedgerAppInstanceFactory::LedgerAppInstance::GetPage(
    const f1dl::Array<uint8_t>& page_id,
    ledger::Status expected_status) {
  ledger::PagePtr page_ptr;
  ledger::Status status;
  ledger::LedgerPtr ledger = GetTestLedger();
  ledger->GetPage(page_id.Clone(), page_ptr.NewRequest(),
                  [&status](ledger::Status s) { status = s; });
  EXPECT_TRUE(ledger.WaitForResponseUntil(zx::deadline_after(kTimeout)));
  EXPECT_EQ(expected_status, status);

  return page_ptr;
}

void LedgerAppInstanceFactory::LedgerAppInstance::DeletePage(
    const f1dl::Array<uint8_t>& page_id,
    ledger::Status expected_status) {
  f1dl::InterfaceHandle<ledger::Page> page;
  ledger::Status status;
  ledger::LedgerPtr ledger = GetTestLedger();
  ledger->DeletePage(page_id.Clone(),
                     [&status](ledger::Status s) { status = s; });
  EXPECT_TRUE(ledger.WaitForResponseUntil(zx::deadline_after(kTimeout)));
  EXPECT_EQ(expected_status, status);
}

}  // namespace test
