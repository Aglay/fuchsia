// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_LEDGER_TESTING_QUIT_ON_ERROR_H_
#define PERIDOT_BIN_LEDGER_TESTING_QUIT_ON_ERROR_H_

#include <functional>
#include <string>

#include <fuchsia/cpp/ledger.h>
#include "lib/fxl/strings/string_view.h"

namespace test {
namespace benchmark {

// Logs an error and posts a quit task on the current message loop if the given
// ledger status is not ledger::Status::OK. Returns true if the quit task was
// posted.
bool QuitOnError(ledger::Status status, fxl::StringView description);

std::function<void(ledger::Status)> QuitOnErrorCallback(
    std::string description);

}  // namespace benchmark
}  // namespace test

#endif  // PERIDOT_BIN_LEDGER_TESTING_QUIT_ON_ERROR_H_
