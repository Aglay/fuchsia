// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <initializer_list>

#include "garnet/bin/zxdb/symbols/type.h"

namespace zxdb {

class BaseType;
class Collection;
class Type;

// Used for declarations that have a name and a type.
using NameAndType = std::pair<std::string, fxl::RefPtr<Type>>;

// Returns a type that can hold 4-byte signed integers.
fxl::RefPtr<BaseType> MakeInt32Type();

// Creates a collection type with the given members.
//
// type_tag is one of Symbol::kTag*Type appropriate for collections (class,
// struct, union).
fxl::RefPtr<Collection> MakeCollectionType(
    int type_tag, const std::string& struct_name,
    std::initializer_list<NameAndType> members);

// Like MakeCollectionType but takes an offset for the first data member to
// start at. Subsequent data members go from there.
fxl::RefPtr<Collection> MakeCollectionTypeWithOffset(
    int type_tag, const std::string& type_name, uint32_t first_member_offset,
    std::initializer_list<NameAndType> members);

// Makes a two collections, one a base class of the other, and returns the
// derived type.
//
// type_tag is one of Symbol::kTag*Type appropriate for collections (class,
// struct, union).
fxl::RefPtr<Collection> MakeDerivedClassPair(
    int type_tag, const std::string& base_name,
    std::initializer_list<NameAndType> base_members,
    const std::string& derived_name,
    std::initializer_list<NameAndType> derived_members);

}  // namespace zxdb
