// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/shell/interpreter/src/expressions.h"

#include <ostream>

#include "src/developer/shell/interpreter/src/nodes.h"
#include "src/developer/shell/interpreter/src/schema.h"
#include "src/developer/shell/interpreter/src/types.h"

namespace shell {
namespace interpreter {

std::unique_ptr<Type> Expression::GetType() const { return std::make_unique<TypeUndefined>(); }

std::unique_ptr<Type> IntegerLiteral::GetType() const { return std::make_unique<TypeInteger>(); }

void IntegerLiteral::Dump(std::ostream& os) const {
  if (negative_) {
    os << '-';
  }
  os << absolute_value_;
}

void IntegerLiteral::Compile(ExecutionContext* context, code::Code* code,
                             const Type* for_type) const {
  for_type->GenerateIntegerLiteral(context, code, this);
}

void ObjectField::Dump(std::ostream& os) const { os << *type_ << " : " << *expression_; }

void Object::Dump(std::ostream& os) const {
  os << "{";
  const char* separator = "";
  for (size_t i = 0; i < fields_.size(); i++) {
    os << separator << *(fields_[i]);
    separator = ", ";
  }
  os << "}";
}

void Object::Compile(ExecutionContext* context, code::Code* code, const Type* for_type) const {
  // TODO: Actually do something when we encounter a object
}

std::unique_ptr<Type> StringLiteral::GetType() const { return std::make_unique<TypeString>(); }

void StringLiteral::Dump(std::ostream& os) const {
  // TODO(vbelliard): escape special characters.
  os << '"' << string()->value() << '"';
}

void StringLiteral::Compile(ExecutionContext* context, code::Code* code,
                            const Type* for_type) const {
  for_type->GenerateStringLiteral(context, code, this);
}

}  // namespace interpreter
}  // namespace shell
