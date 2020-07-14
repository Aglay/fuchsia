// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

#include "src/developer/debug/debug_agent/zircon_thread_exception.h"

#include "src/developer/debug/debug_agent/zircon_thread_handle.h"

namespace debug_agent {

std::unique_ptr<ThreadHandle> ZirconThreadException::GetThreadHandle() const {
  zx::thread thread;
  if (zx_status_t status = exception_.get_thread(&thread); status != ZX_OK)
    return nullptr;
  return std::make_unique<ZirconThreadHandle>(std::move(thread));
}

fitx::result<zx_status_t, uint32_t> ZirconThreadException::GetState() const {
  uint32_t state = 0;
  zx_status_t status = exception_.get_property(ZX_PROP_EXCEPTION_STATE, &state, sizeof(state));
  if (status != ZX_OK) {
    return fitx::error(status);
  }
  return fitx::ok(state);
}

zx_status_t ZirconThreadException::SetState(uint32_t state) {
  return exception_.set_property(ZX_PROP_EXCEPTION_STATE, &state, sizeof(state));
}

fitx::result<zx_status_t, uint32_t> ZirconThreadException::GetStrategy() const {
  uint32_t strategy = 0;
  zx_status_t status =
      exception_.get_property(ZX_PROP_EXCEPTION_STRATEGY, &strategy, sizeof(strategy));
  if (status != ZX_OK) {
    return fitx::error(status);
  }
  return fitx::ok(strategy);
}

zx_status_t ZirconThreadException::SetStrategy(uint32_t strategy) {
  return exception_.set_property(ZX_PROP_EXCEPTION_STRATEGY, &strategy, sizeof(strategy));
}

}  // namespace debug_agent
