// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/lib/testing/ledger_repository_for_testing.h"

#include <utility>

#include <fuchsia/modular/cpp/fidl.h>
#include "peridot/lib/common/teardown.h"
#include "peridot/lib/fidl/app_client.h"
#include "peridot/lib/ledger_client/constants.h"

namespace fuchsia {
namespace modular {

namespace testing {

LedgerRepositoryForTesting::LedgerRepositoryForTesting()
    : startup_context_(fuchsia::sys::StartupContext::CreateFromStartupInfo()) {
  AppConfig ledger_config;
  ledger_config.url = kLedgerAppUrl;
  ledger_config.args.push_back(kLedgerNoMinfsWaitFlag);

  auto& app_launcher = startup_context_->launcher();
  ledger_app_client_ =
      std::make_unique<AppClient<ledger_internal::LedgerController>>(
          app_launcher.get(), std::move(ledger_config));

  ledger_app_client_->services().ConnectToService(
      ledger_repo_factory_.NewRequest());
}

LedgerRepositoryForTesting::~LedgerRepositoryForTesting() = default;

ledger_internal::LedgerRepository*
LedgerRepositoryForTesting::ledger_repository() {
  if (!ledger_repo_) {
    ledger_repo_factory_->GetRepository(
        tmp_dir_.path(), nullptr, ledger_repo_.NewRequest(),
        [this](ledger::Status status) {
          FXL_CHECK(status == ledger::Status::OK);
        });
  }

  return ledger_repo_.get();
}

void LedgerRepositoryForTesting::Terminate(std::function<void()> done) {
  if (ledger_app_client_) {
    ledger_app_client_->Teardown(kBasicTimeout, [this, done] {
      ledger_repo_factory_.Unbind();
      ledger_app_client_.reset();
      done();
    });

  } else {
    done();
  }
}

}  // namespace testing
}  // namespace modular
}  // namespace fuchsia
