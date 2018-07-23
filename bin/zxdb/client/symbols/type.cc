// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/zxdb/client/symbols/type.h"

#include "lib/fxl/logging.h"

namespace zxdb {

Type::Type(int kind) : Symbol(kind) {}
Type::~Type() = default;

const Type* Type::AsType() const { return this; }

const std::string& Type::GetTypeName() const {
  // This base type class just uses the assigned name for the type name.
  // Derived classes will override this function to apply modifiers.
  return assigned_name_;
}

}  // namespace zxdb
