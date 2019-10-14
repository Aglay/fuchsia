// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

#include "src/developer/debug/debug_agent/limbo_provider.h"

#include <zircon/status.h>

using namespace fuchsia::exception;

namespace debug_agent {

LimboProvider::LimboProvider(std::shared_ptr<sys::ServiceDirectory> services)
    : services_(std::move(services)) {}
LimboProvider::~LimboProvider() = default;

zx_status_t LimboProvider::ListProcessesOnLimbo(std::vector<ProcessExceptionMetadata>* out) {
  // We connect synchronously to the limbo service.
  ProcessLimboSyncPtr process_limbo;
  zx_status_t status = services_->Connect(process_limbo.NewRequest());
  if (status != ZX_OK)
    return status;

  return process_limbo->ListProcessesWaitingOnException(out);
}

}  // namespace debug_agent
