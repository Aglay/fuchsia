// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ledger/lib/coroutine/coroutine_waiter.h"

#include <lib/fit/function.h>

#include "gtest/gtest.h"
#include "src/ledger/lib/coroutine/coroutine_impl.h"
#include "src/lib/callback/waiter.h"

namespace coroutine {
namespace {
TEST(CoroutineWaiterTest, Wait) {
  CoroutineServiceImpl coroutine_service;

  fit::closure on_done;
  coroutine_service.StartCoroutine([&](CoroutineHandler* current_handler) {
    auto waiter = fxl::MakeRefCounted<callback::CompletionWaiter>();
    on_done = waiter->NewCallback();
    EXPECT_EQ(ContinuationStatus::OK, Wait(current_handler, std::move(waiter)));
  });

  on_done();
}
}  // namespace
}  // namespace coroutine
