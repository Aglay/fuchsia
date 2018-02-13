// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/tests/benchmark/story/tracing_waiter.h"

#include "lib/fsl/tasks/message_loop.h"

namespace modular {

TracingWaiter::TracingWaiter() = default;
TracingWaiter::~TracingWaiter() = default;

void TracingWaiter::WaitForTracing(std::function<void()> cont) {
  auto* const loop = fsl::MessageLoop::GetCurrent();

  // Cf. RunWithTracing() used by ledger benchmarks.
  trace_provider_ = std::make_unique<trace::TraceProvider>(loop->async());
  trace_observer_ = std::make_unique<trace::TraceObserver>();

  std::function<void()> on_trace_state_changed = [this, cont] {
    if (TRACE_CATEGORY_ENABLED("benchmark") && !started_) {
      started_ = true;
      cont();
    }
  };

  // In case tracing has already started.
  on_trace_state_changed();

  if (!started_) {
    trace_observer_->Start(loop->async(), on_trace_state_changed);
  }
}

}  // namespace modular
