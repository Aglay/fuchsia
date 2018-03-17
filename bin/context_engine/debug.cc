// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/context_engine/debug.h"
#include "peridot/bin/context_engine/context_repository.h"

namespace maxwell {

ContextDebugImpl::ContextDebugImpl(const ContextRepository* const repository)
    : repository_(repository) {}
ContextDebugImpl::~ContextDebugImpl() = default;

void ContextDebugImpl::OnValueChanged(const std::set<Id>& parent_ids,
                                      const Id& id,
                                      const ContextValuePtr& value) {
  auto update = ContextDebugValue::New();
  update->parent_ids = f1dl::Array<f1dl::String>::From(parent_ids);
  update->id = id;
  update->value = value.Clone();
  DispatchOneValue(std::move(update));
}

void ContextDebugImpl::OnValueRemoved(const Id& id) {
  auto update = ContextDebugValue::New();
  update->id = id;
  update->parent_ids = f1dl::Array<f1dl::String>::New(0);
  DispatchOneValue(std::move(update));
}

void ContextDebugImpl::OnSubscriptionAdded(
    const Id& id,
    const ContextQueryPtr& query,
    const SubscriptionDebugInfoPtr& debug_info) {
  auto update = ContextDebugSubscription::New();
  update->id = id;
  update->query = query.Clone();
  update->debug_info = debug_info.Clone();
  DispatchOneSubscription(std::move(update));
}

void ContextDebugImpl::OnSubscriptionRemoved(const Id& id) {
  auto update = ContextDebugSubscription::New();
  update->id = id;
  DispatchOneSubscription(std::move(update));
}

void ContextDebugImpl::Watch(
    f1dl::InterfaceHandle<ContextDebugListener> listener) {
  FXL_LOG(INFO) << "Watch(): entered";
  auto listener_ptr = listener.Bind();
  // Build a complete state snapshot and send it to |listener|.
  auto all_values = f1dl::Array<ContextDebugValuePtr>::New(0);
  for (const auto& entry : repository_->values_) {
    auto update = ContextDebugValue::New();
    update->id = entry.first;
    update->value = entry.second.value.Clone();
    update->parent_ids = f1dl::Array<f1dl::String>::From(
        repository_->graph_.GetParents(entry.first));
    all_values.push_back(std::move(update));
  }
  listener_ptr->OnValuesChanged(std::move(all_values));
  // TODO(thatguy): Add subscriptions.

  listeners_.AddInterfacePtr(std::move(listener_ptr));
}

void ContextDebugImpl::DispatchOneValue(ContextDebugValuePtr value) {
  f1dl::Array<ContextDebugValuePtr> values;
  values.push_back(value.Clone());
  DispatchValues(std::move(values));
}

void ContextDebugImpl::DispatchValues(
    f1dl::Array<ContextDebugValuePtr> values) {
  listeners_.ForAllPtrs([&values](ContextDebugListener* listener) {
    listener->OnValuesChanged(values.Clone());
  });
}

void ContextDebugImpl::DispatchOneSubscription(
    ContextDebugSubscriptionPtr value) {
  f1dl::Array<ContextDebugSubscriptionPtr> values;
  values.push_back(value.Clone());
  DispatchSubscriptions(std::move(values));
}

void ContextDebugImpl::DispatchSubscriptions(
    f1dl::Array<ContextDebugSubscriptionPtr> subscriptions) {
  listeners_.ForAllPtrs([&subscriptions](ContextDebugListener* listener) {
    listener->OnSubscriptionsChanged(subscriptions.Clone());
  });
}

}  // namespace maxwell
