// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_CONTEXT_ENGINE_DEBUG_H_
#define PERIDOT_BIN_CONTEXT_ENGINE_DEBUG_H_

#include <map>

#include "lib/context/fidl/debug.fidl.h"
#include "lib/fidl/cpp/bindings/interface_ptr_set.h"
#include "peridot/bin/context_engine/index.h"

namespace maxwell {

class ContextRepository;

class ContextDebugImpl : public ContextDebug {
  using Id = ContextIndex::Id;

 public:
  ContextDebugImpl(const ContextRepository* repository);
  ~ContextDebugImpl();

  void OnValueChanged(const std::set<Id>& parent_ids,
                      const Id& id,
                      const ContextValuePtr& value);
  void OnValueRemoved(const Id& id);

  void OnSubscriptionAdded(const Id& id,
                           const ContextQueryPtr& query,
                           const SubscriptionDebugInfoPtr& debug_info);
  void OnSubscriptionRemoved(const Id& id);

 private:
  // |ContextDebug|
  void Watch(f1dl::InterfaceHandle<ContextDebugListener> listener) override;

  void DispatchOneValue(ContextDebugValuePtr value);
  void DispatchValues(f1dl::VectorPtr<ContextDebugValuePtr> values);
  void DispatchOneSubscription(ContextDebugSubscriptionPtr value);
  void DispatchSubscriptions(f1dl::VectorPtr<ContextDebugSubscriptionPtr> values);

  // Used in order to get a complete state snapshot when Watch() is called.
  const ContextRepository* const repository_;
  f1dl::InterfacePtrSet<ContextDebugListener> listeners_;
};

}  // namespace maxwell

#endif  // PERIDOT_BIN_CONTEXT_ENGINE_DEBUG_H_
