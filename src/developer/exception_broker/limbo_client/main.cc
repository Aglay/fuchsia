// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/sys/cpp/component_context.h>
#include <stdio.h>
#include <zircon/status.h>

#include "src/developer/exception_broker/limbo_client/limbo_client.h"

using namespace fuchsia::exception;

namespace {

void PrintError(zx_status_t status) {
  fprintf(stderr, "Could not communicate to limbo: %s\n", zx_status_get_string(status));
}

};  // namespace

int main() {
  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);

  auto context = sys::ComponentContext::Create();
  auto& services = context->svc();

  LimboClient client(services);
  if (zx_status_t status = client.Init(); status != ZX_OK) {
    PrintError(status);
    return EXIT_FAILURE;
  }

  printf("Is limbo active? %d\n", client.active());

  return EXIT_SUCCESS;
}
