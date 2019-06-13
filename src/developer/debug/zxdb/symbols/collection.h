// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_COLLECTION_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_COLLECTION_H_

#include "src/developer/debug/zxdb/symbols/type.h"

namespace zxdb {

// Represents a C/C++ class, struct, or union, or a Rust enum (see the
// variant_part() member).
class Collection final : public Type {
 public:
  // Construct with fxl::MakeRefCounted().

  // Symbol overrides.
  const Collection* AsCollection() const override;

  // Data members. These should be DataMember objects.
  const std::vector<LazySymbol>& data_members() const { return data_members_; }
  void set_data_members(std::vector<LazySymbol> d) {
    data_members_ = std::move(d);
  }

  // This will be a VariantPart class if there is one defined.
  //
  // Currently this is used only for Rust enums. In this case, the collection
  // will contain one VariantPart (the Variants inside of it will encode the
  // enumerated possibilities) and this collection will have no data_members()
  // in its vector. See the VariantPart declaration for more details.
  //
  // Theoretically DWARF could encode more than one variant part child of a
  // struct but none of our supported compilers or languages do this so we
  // save as a single value.
  const LazySymbol& variant_part() const { return variant_part_; }
  void set_variant_part(const LazySymbol& vp) { variant_part_ = vp; }

  // Classes/structs this one inherits from. This should be a InheritedFrom
  // object.
  //
  // These are in the same order as declared in the symbol file.
  const std::vector<LazySymbol>& inherited_from() const {
    return inherited_from_;
  }
  void set_inherited_from(std::vector<LazySymbol> f) {
    inherited_from_ = std::move(f);
  }

  // Returns a pointer to either "struct", "class", or "union" depending on the
  // type of this object. This is useful for error messages.
  const char* GetKindString() const;

  // Currently we don't have any notion of member functions because there's
  // no need. That could be added here if necessary (generally the symbols
  // will contain this).

 private:
  FRIEND_REF_COUNTED_THREAD_SAFE(Collection);
  FRIEND_MAKE_REF_COUNTED(Collection);

  Collection(DwarfTag kind, std::string name = std::string());
  virtual ~Collection();

  // Symbol protected overrides.
  std::string ComputeFullName() const override;

  std::vector<LazySymbol> data_members_;
  LazySymbol variant_part_;
  std::vector<LazySymbol> inherited_from_;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_COLLECTION_H_
