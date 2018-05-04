// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_LEDGER_FIDL_HELPERS_BOUNDABLE_H_
#define PERIDOT_BIN_LEDGER_FIDL_HELPERS_BOUNDABLE_H_

#include "lib/fidl/cpp/binding.h"
#include "lib/fxl/macros.h"

namespace ledger {
namespace fidl_helpers {
// Represents an object that can be bound to once.
template <class Interface>
class Boundable {
 public:
  virtual ~Boundable() = default;

  // Binds a single interface request to the object.
  virtual void Bind(fidl::InterfaceRequest<Interface> request) = 0;
};

// Represents an object that can be bound to multiple times.
template <class Interface>
class SetBoundable {
 public:
  virtual ~SetBoundable() = default;

  // Adds a binding to the object.
  virtual void AddBinding(fidl::InterfaceRequest<Interface> request) = 0;
};
}  // namespace fidl_helpers
}  // namespace ledger

#endif  // PERIDOT_BIN_LEDGER_FIDL_HELPERS_BOUNDABLE_H_
