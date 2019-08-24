// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MODULAR_LIB_CONTEXT_CPP_CONTEXT_HELPER_H_
#define SRC_MODULAR_LIB_CONTEXT_CPP_CONTEXT_HELPER_H_

#include <fuchsia/modular/cpp/fidl.h>
#include <lib/fidl/cpp/vector.h>

#include <utility>

namespace modular {

// Takes a single value specified by |key| from |update|, and sets
// |update.values| to null."
std::optional<std::vector<fuchsia::modular::ContextValue>> TakeContextValue(
    fuchsia::modular::ContextUpdate* update, const std::string& key);

void AddToContextQuery(fuchsia::modular::ContextQuery* query, const std::string& key,
                       fuchsia::modular::ContextSelector selector);

bool HasSelectorKey(fuchsia::modular::ContextQuery* const query, const std::string& key);

}  // namespace modular

#endif  // SRC_MODULAR_LIB_CONTEXT_CPP_CONTEXT_HELPER_H_
