// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <iostream>

#include "apps/tracing/lib/trace/internal/fields.h"
#include "apps/tracing/src/trace_manager/trace_manager.h"
#include "lib/mtl/tasks/message_loop.h"

using namespace tracing::internal;

namespace tracing {
namespace {

const ftl::TimeDelta kStopTimeout = ftl::TimeDelta::FromSeconds(5);
static constexpr size_t kTraceBufferSize = 3 * 1024 * 1024;

std::string SanitizeLabel(const fidl::String& label) {
  std::string result =
      label.get().substr(0, tracing::TraceRegistry::kLabelMaxLength);
  if (result.empty())
    result = "unnamed";
  return result;
}

}  // namespace

TraceManager::TraceManager(const Config& config) : config_(config) {}

TraceManager::~TraceManager() = default;

void TraceManager::StartTracing(fidl::Array<fidl::String> categories,
                                mx::socket output) {
  if (session_) {
    FTL_LOG(ERROR) << "Trace already in progress";
    return;
  }

  FTL_VLOG(1) << "Starting trace";

  session_ = ftl::MakeRefCounted<TraceSession>(
      std::move(output), std::move(categories), kTraceBufferSize,
      [this]() { session_ = nullptr; });

  for (auto& bundle : providers_) {
    FTL_VLOG(1) << "  for provider " << bundle;
    session_->AddProvider(&bundle);
  }
}

void TraceManager::StopTracing() {
  if (!session_)
    return;

  FTL_VLOG(1) << "Stopping trace";
  session_->Stop(
      [this]() {
        FTL_VLOG(1) << "Stopped trace";
        session_ = nullptr;
      },
      kStopTimeout);
}

void TraceManager::GetKnownCategories(
    const GetKnownCategoriesCallback& callback) {
  callback(
      fidl::Map<fidl::String, fidl::String>::From(config_.known_categories));
}

void TraceManager::GetRegisteredProviders(
    const GetRegisteredProvidersCallback& callback) {
  fidl::Array<TraceProviderInfoPtr> results;
  results.resize(0u);
  for (const auto& provider : providers_) {
    auto info = TraceProviderInfo::New();
    info->label = provider.label;
    info->id = provider.id;
    results.push_back(std::move(info));
  }
  callback(std::move(results));
}

void TraceManager::RegisterTraceProvider(
    fidl::InterfaceHandle<TraceProvider> handle,
    const fidl::String& label) {
  FTL_VLOG(1) << "Registering provider with label: " << label;

  auto it = providers_.emplace(
      providers_.end(),
      TraceProviderBundle{TraceProviderPtr::Create(std::move(handle)),
                          next_provider_id_++, SanitizeLabel(label)});

  it->provider.set_connection_error_handler([this, it]() {
    if (session_)
      session_->RemoveDeadProvider(&(*it));
    providers_.erase(it);
  });

  if (session_)
    session_->AddProvider(&(*it));
}

}  // namespace tracing
