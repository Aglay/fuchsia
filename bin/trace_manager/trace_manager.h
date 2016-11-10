// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APPS_TRACING_SRC_TRACE_MANAGER_TRACE_MANAGER_H_
#define APPS_TRACING_SRC_TRACE_MANAGER_TRACE_MANAGER_H_

#include <atomic>
#include <chrono>
#include <memory>
#include <unordered_map>

#include "apps/tracing/services/trace_manager.fidl.h"
#include "lib/fidl/cpp/bindings/binding_set.h"
#include "lib/fidl/cpp/bindings/interface_ptr_set.h"
#include "lib/fidl/cpp/bindings/interface_request.h"
#include "lib/ftl/macros.h"

namespace tracing {

class TraceManager : public TraceRegistry, public TraceController {
 public:
  TraceManager();
  ~TraceManager() override;

 private:
  enum class ControllerState { kStarted, kStopped };

  // |TraceController| implementation.
  void StartTracing(fidl::Array<fidl::String> categories,
                    mx::datapipe_producer output) override;
  void StopTracing() override;

  // |TraceRegistry| implementation.
  void RegisterTraceProvider(
      fidl::InterfaceHandle<tracing::TraceProvider> provider,
      const fidl::String& label,
      fidl::Map<fidl::String, fidl::String> categories) override;

  ControllerState controller_state_{ControllerState::kStopped};

  fidl::Array<fidl::String> categories_;
  fidl::InterfacePtrSet<TraceProvider> trace_providers_;
  FTL_DISALLOW_COPY_AND_ASSIGN(TraceManager);
};

}  // namespace tracing

#endif  // APPS_TRACING_SRC_TRACE_MANAGER_TRACE_MANAGER_H_
