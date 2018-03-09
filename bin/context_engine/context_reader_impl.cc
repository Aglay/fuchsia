// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/context_engine/context_reader_impl.h"

#include "lib/context/cpp/formatting.h"
#include "lib/context/fidl/debug.fidl.h"
#include "peridot/bin/context_engine/context_repository.h"

namespace maxwell {

ContextReaderImpl::ContextReaderImpl(
    ComponentScopePtr client_info,
    ContextRepository* repository,
    f1dl::InterfaceRequest<ContextReader> request)
    : binding_(this, std::move(request)), repository_(repository) {
  debug_ = SubscriptionDebugInfo::New();
  debug_->client_info = std::move(client_info);
}

ContextReaderImpl::~ContextReaderImpl() = default;

void ContextReaderImpl::Subscribe(
    ContextQueryPtr query,
    f1dl::InterfaceHandle<ContextListener> listener) {
  auto listener_ptr = listener.Bind();
  repository_->AddSubscription(std::move(query), std::move(listener_ptr),
                               debug_.Clone());
}

void ContextReaderImpl::Get(
    ContextQueryPtr query,
    const GetCallback& callback) {
  callback(repository_->Query(query));
}

}  // namespace maxwell
