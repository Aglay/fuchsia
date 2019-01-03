// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/zxdb/symbols/modified_type.h"

#include "garnet/bin/zxdb/symbols/arch.h"
#include "garnet/bin/zxdb/symbols/function_type.h"

namespace zxdb {

namespace {

// Returns true if this tag is a modified type that is transparent with respect
// to the data stored in it.
bool IsTransparentTag(int tag) {
  return tag == Symbol::kTagConstType || tag == Symbol::kTagVolatileType ||
         tag == Symbol::kTagTypedef || tag == Symbol::kTagRestrictType;
}

// Returns true if this modified holds some kind of pointer to the modified
// type.
bool IsPointerTag(int tag) {
  return tag == Symbol::kTagPointerType || tag == Symbol::kTagReferenceType ||
         tag == Symbol::kTagRvalueReferenceType;
}

}  // namespace

ModifiedType::ModifiedType(int kind, LazySymbol modified)
    : Type(kind), modified_(modified) {
  if (IsTransparentTag(kind)) {
    const Type* mod_type = modified_.Get()->AsType();
    if (mod_type)
      set_byte_size(mod_type->byte_size());
  } else if (IsPointerTag(kind)) {
    set_byte_size(kTargetPointerSize);
  }
}

ModifiedType::~ModifiedType() = default;

const ModifiedType* ModifiedType::AsModifiedType() const { return this; }

const Type* ModifiedType::GetConcreteType() const {
  if (IsTransparentTag(tag())) {
    const Type* mod = modified_.Get()->AsType();
    if (mod)
      return mod->GetConcreteType();
  }
  return this;
}

// static
bool ModifiedType::IsTypeModifierTag(int tag) {
  return tag == kTagConstType || tag == kTagPointerType ||
         tag == kTagReferenceType || tag == kTagRestrictType ||
         tag == kTagRvalueReferenceType || tag == kTagTypedef ||
         tag == kTagVolatileType || tag == kTagImportedDeclaration;
}

std::string ModifiedType::ComputeFullName() const {
  static const char kUnknown[] = "<unknown>";

  // Typedefs are special and just use the assigned name. Every other modifier
  // below is based on the underlying type name.
  if (tag() == kTagTypedef)
    return GetAssignedName();

  const Type* modified_type = nullptr;
  std::string modified_name;
  if (!modified()) {
    // No modified type means "void".
    modified_name = "void";
  } else {
    if (auto func_type = modified().Get()->AsFunctionType();
        func_type && tag() == kTagPointerType) {
      // Special-case pointer-to-funcion which has unusual syntax.
      // TODO(DX-683) this doesn't handle pointers of references to
      // pointers-to-member functions
      return func_type->ComputeFullNameForFunctionPtr(std::string());
    } else if ((modified_type = modified().Get()->AsType())) {
      // All other types.
      modified_name = modified_type->GetFullName();
    } else {
      // Symbols likely corrupt.
      modified_name = kUnknown;
    }
  }

  switch (tag()) {
    case kTagConstType:
      if (modified_type && modified_type->AsModifiedType()) {
        // When the underlying type is another modifier, add it to the end,
        // e.g. a "constant pointer to a nonconstant int" is "int* const".
        return modified_name + " const";
      } else {
        // Though the above formatting is always valid, most people write a
        // "constant int" / "pointer to a constant int" as either "const int" /
        // "const int*" so special-case.
        return "const " + modified_name;
      }
    case kTagPointerType:
      return modified_name + "*";
    case kTagReferenceType:
      return modified_name + "&";
    case kTagRestrictType:
      return modified_name + " restrict";
    case kTagRvalueReferenceType:
      return modified_name + "&&";
    case kTagVolatileType:
      return "volatile " + modified_name;
    case kTagImportedDeclaration:
      // Using statements use the underlying name.
      return modified_name;
  }
  return kUnknown;
}

}  // namespace zxdb
