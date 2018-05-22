// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/ledger/testing/get_ledger.h"

#include <utility>

#include <cloud_provider_firebase/cpp/fidl.h>
#include <ledger_internal/cpp/fidl.h>
#include <lib/async/cpp/task.h>

#include "lib/callback/capture.h"
#include "lib/fxl/functional/make_copyable.h"
#include "lib/fxl/logging.h"
#include "lib/svc/cpp/services.h"
#include "peridot/lib/convert/convert.h"

namespace test {
namespace {
constexpr zx::duration kTimeout = zx::sec(10);
}  // namespace

ledger::Status GetLedger(fxl::Closure quit_callback,
                         component::ApplicationContext* context,
                         component::ApplicationControllerPtr* controller,
                         cloud_provider::CloudProviderPtr cloud_provider,
                         std::string ledger_name,
                         std::string ledger_repository_path,
                         ledger::LedgerPtr* ledger_ptr) {
  ledger_internal::LedgerRepositoryFactoryPtr repository_factory;
  component::Services child_services;
  component::LaunchInfo launch_info;
  launch_info.url = "ledger";
  launch_info.directory_request = child_services.NewRequest();
  launch_info.arguments.push_back("--no_minfs_wait");
  launch_info.arguments.push_back("--no_statistics_reporting_for_testing");

  context->launcher()->CreateApplication(std::move(launch_info),
                                         controller->NewRequest());
  child_services.ConnectToService(repository_factory.NewRequest());
  ledger_internal::LedgerRepositoryPtr repository;

  ledger::Status status = ledger::Status::UNKNOWN_ERROR;

  repository_factory->GetRepository(
      ledger_repository_path, std::move(cloud_provider),
      repository.NewRequest(), callback::Capture([] {}, &status));
  if (repository_factory.WaitForResponseUntil(zx::deadline_after(kTimeout)) !=
      ZX_OK) {
    FXL_LOG(ERROR) << "Unable to get repository.";
    return ledger::Status::INTERNAL_ERROR;
  }
  if (status != ledger::Status::OK) {
    FXL_LOG(ERROR) << "Failure while getting repository.";
    return status;
  }

  repository->GetLedger(convert::ToArray(ledger_name), ledger_ptr->NewRequest(),
                        callback::Capture([] {}, &status));
  if (repository.WaitForResponseUntil(zx::deadline_after(kTimeout)) != ZX_OK) {
    FXL_LOG(ERROR) << "Unable to get ledger.";
    return ledger::Status::INTERNAL_ERROR;
  }
  if (status != ledger::Status::OK) {
    FXL_LOG(ERROR) << "Failure while getting ledger.";
    return status;
  }
  ledger_ptr->set_error_handler([quit_callback = std::move(quit_callback)] {
    FXL_LOG(ERROR) << "The ledger connection was closed, quitting.";
    quit_callback();
  });

  return status;
}

ledger::Status GetPageEnsureInitialized(fxl::Closure quit_callback,
                                        ledger::LedgerPtr* ledger,
                                        ledger::PageIdPtr requested_id,
                                        ledger::PagePtr* page,
                                        ledger::PageId* page_id) {
  ledger::Status status;
  (*ledger)->GetPage(std::move(requested_id), page->NewRequest(),
                     callback::Capture([] {}, &status));
  if (ledger->WaitForResponseUntil(zx::deadline_after(kTimeout)) != ZX_OK) {
    FXL_LOG(ERROR) << "Unable to get page.";
    return ledger::Status::INTERNAL_ERROR;
  }
  if (status != ledger::Status::OK) {
    return status;
  }

  page->set_error_handler([quit_callback = std::move(quit_callback)] {
    FXL_LOG(ERROR) << "The page connection was closed, quitting.";
    quit_callback();
  });

  (*page)->GetId(callback::Capture([] {}, page_id));
  page->WaitForResponseUntil(zx::deadline_after(kTimeout));
  return status;
}

}  // namespace test
