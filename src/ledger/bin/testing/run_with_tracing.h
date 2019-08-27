// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LEDGER_BIN_TESTING_RUN_WITH_TRACING_H_
#define SRC_LEDGER_BIN_TESTING_RUN_WITH_TRACING_H_

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/fit/function.h>

#include <functional>

namespace ledger {

// Adds a TraceObserver to start running |runnable| as soon as the tracing is
// enabled; then runs the message loop |loop|.
// If tracing is still not enabled after 5 seconds, posts a quit task.
int RunWithTracing(async::Loop* loop, fit::function<void()> runnable);

}  // namespace ledger

#endif  // SRC_LEDGER_BIN_TESTING_RUN_WITH_TRACING_H_
