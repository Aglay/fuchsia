// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_LEDGER_TESTING_GET_LEDGER_H_
#define PERIDOT_BIN_LEDGER_TESTING_GET_LEDGER_H_

#include <functional>
#include <string>

#include <fuchsia/cpp/cloud_provider.h>
#include <fuchsia/cpp/ledger.h>
#include <fuchsia/cpp/modular_auth.h>
#include <lib/async-loop/cpp/loop.h>

#include "lib/app/cpp/application_context.h"
#include "lib/fxl/functional/closure.h"
#include "lib/fxl/memory/ref_ptr.h"
#include "lib/fxl/strings/string_view.h"
#include "peridot/bin/ledger/fidl_helpers/boundable.h"

namespace test {

// Creates a new Ledger application instance and returns a LedgerPtr connection
// to it.
//
// TODO(ppi): take the server_id as std::optional<std::string> and drop bool
// sync once we're on C++17.
ledger::Status GetLedger(fxl::Closure quit_callback,
                         component::ApplicationContext* context,
                         component::ApplicationControllerPtr* controller,
                         cloud_provider::CloudProviderPtr cloud_provider,
                         std::string ledger_name,
                         std::string ledger_repository_path,
                         ledger::LedgerPtr* ledger_ptr);

// Retrieves the requested page of the given Ledger instance and calls the
// callback only after executing a GetId() call on the page, ensuring that it is
// already initialized. If |id| is nullptr, a new page with a unique id is
// created.
ledger::Status GetPageEnsureInitialized(fxl::Closure quit_callback,
                                        ledger::LedgerPtr* ledger,
                                        ledger::PageIdPtr requested_id,
                                        ledger::PagePtr* page,
                                        ledger::PageId* page_id);

}  // namespace test

#endif  // PERIDOT_BIN_LEDGER_TESTING_GET_LEDGER_H_
