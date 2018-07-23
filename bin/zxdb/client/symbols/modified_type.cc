// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/zxdb/client/symbols/modified_type.h"

namespace zxdb {

ModifiedType::ModifiedType(int kind) : Type(kind) {}
ModifiedType::~ModifiedType() = default;

const ModifiedType* ModifiedType::AsModifiedType() const { return this; }

const std::string& ModifiedType::GetTypeName() const {
  if (!computed_type_name_) {
    type_name_ = ComputeTypeName();
    computed_type_name_ = true;
  }
  return type_name_;
}

// static
bool ModifiedType::IsTypeModifierTag(int tag) {
  return tag == kTagConstType || tag == kTagPointerType ||
         tag == kTagReferenceType || tag == kTagRestrictType ||
         tag == kTagRvalueReferenceType || tag == kTagTypedef ||
         tag == kTagVolatileType || tag == kTagImportedDeclaration;
}

std::string ModifiedType::ComputeTypeName() const {
  static const char kUnknown[] = "unknown";
  const Type* modified_type = modified().Get()->AsType();
  if (!modified_type)
    return kUnknown;

  switch (tag()) {
    case kTagConstType:
      if (modified_type->AsModifiedType()) {
        // When the underlying type is another modifier, add it to the end,
        // e.g. a "constant pointer to a nonconstant int" is "int* const".
        return modified_type->GetTypeName() + " const";
      } else {
        // Though the above formatting is always valid, most people write a
        // "constant int" / "pointer to a constant int" as either "const int" /
        // "const int*" so special-case.
        return "const " + modified_type->GetTypeName();
      }
    case kTagPointerType:
      return modified_type->GetTypeName() + "*";
    case kTagReferenceType:
      return modified_type->GetTypeName() + "&";
    case kTagRestrictType:
      return "restrict " + modified_type->GetTypeName();
    case kTagRvalueReferenceType:
      return modified_type->GetTypeName() + "&&";
    case kTagTypedef:
      // Typedefs just use the assigned name.
      return assigned_name();
    case kTagVolatileType:
      return "volatile " + modified_type->GetTypeName();
    case kTagImportedDeclaration:
      // Using statements use the underlying name.
      return modified_type->GetTypeName();
  }
  return kUnknown;
}

}  // namespace zxdb
