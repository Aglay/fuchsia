// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/trace/tests/component_context.h"

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>

#include <memory>

#include "src/lib/fxl/logging.h"

namespace tracing {
namespace test {

static std::unique_ptr<sys::ComponentContext> g_context;

void InitComponentContext() {
  // |Create()| needs a loop, it uses the default dispatcher.
  {
    async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);
    g_context = sys::ComponentContext::CreateAndServeOutgoingDirectory();
    FXL_CHECK(g_context);
  }
}

sys::ComponentContext* GetComponentContext() { return g_context.get(); }

}  // namespace test
}  // namespace tracing
