// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/expr/index_walker2.h"

#include <ctype.h>
#include <string.h>

#include <string_view>

#include "src/developer/debug/zxdb/common/err.h"
#include "src/developer/debug/zxdb/expr/expr_parser.h"
#include "src/developer/debug/zxdb/symbols/index2.h"
#include "src/developer/debug/zxdb/symbols/index_node2.h"
#include "src/lib/fxl/logging.h"

namespace zxdb {

namespace {

// We don't expect to have identifiers with whitespace in them. If somebody does "Foo < Bar>" stop
// considering the name at the space.
inline bool IsNameEnd(char ch) { return isspace(ch) || ch == '<'; }

}  // namespace

IndexWalker2::IndexWalker2(const Index2* index) {
  // Prefer not to reallocate the vector-of-vectors. It is rare for C++ namespace hierarchies to
  // be more than a couple of components long, so this number should cover most cases.
  path_.reserve(8);

  path_.push_back({&index->root()});
}

IndexWalker2::~IndexWalker2() = default;

bool IndexWalker2::WalkUp() {
  if (path_.size() > 1) {
    // Don't walk above the root.
    path_.resize(path_.size() - 1);
    return true;
  }
  return false;
}

bool IndexWalker2::WalkInto(const ParsedIdentifierComponent& comp) {
  const Stage& old_stage = path_.back();

  const std::string& comp_name = comp.name();
  if (comp_name.empty())
    return true;  // No-op.

  Stage new_stage;
  for (const auto* old_node : old_stage) {
    for (int i = 0; i < static_cast<int>(IndexNode2::Kind::kEndPhysical); i++) {
      const IndexNode2::Map& map = old_node->MapForKind(static_cast<IndexNode2::Kind>(i));

      // This is complicated by templates which can't be string-compared for equality without
      // canonicalization. Search everything in the index with the same base (non-template-part)
      // name. With the index being sorted, we can start at the item that begins lexicographically
      // >= the input.
      auto iter = map.lower_bound(comp_name);
      if (iter == map.end())
        continue;  // Nothing can match of this kind.

      if (!comp.has_template()) {
        // In the common case there is no template in the input, so we can just check for exact
        // string equality and be done with this kind.
        if (iter->first == comp_name)
          new_stage.push_back(&iter->second);
        continue;
      }

      // Check all nodes until template canonicalization can't affect the comparison.
      while (iter != map.end() && !IsIndexStringBeyondName(iter->first, comp_name)) {
        if (ComponentMatches(iter->first, comp)) {
          // Found match.
          new_stage.push_back(&iter->second);
          break;
        }

        ++iter;
      }
    }
  }

  if (new_stage.empty())
    return false;  // No children found.

  // Commit the new found stuff.
  path_.push_back(std::move(new_stage));
  return true;
}

bool IndexWalker2::WalkInto(const ParsedIdentifier& ident) {
  IndexWalker2 sub(*this);
  if (!sub.WalkIntoClosest(ident))
    return false;

  // Full walk succeeded, commit.
  std::swap(path_, sub.path_);
  return true;
}

bool IndexWalker2::WalkIntoClosest(const ParsedIdentifier& ident) {
  if (ident.qualification() == IdentifierQualification::kGlobal)
    path_.resize(1);  // Only keep the root.

  for (const auto& comp : ident.components()) {
    if (!WalkInto(comp))
      return false;  // This component not found.
  }
  return true;
}

// static
bool IndexWalker2::ComponentMatches(const std::string& index_string,
                                    const ParsedIdentifierComponent& comp) {
  if (!ComponentMatchesNameOnly(index_string, comp))
    return false;
  // Only bother with the expensive template comparison on demand.
  return ComponentMatchesTemplateOnly(index_string, comp);
}

// static
bool IndexWalker2::ComponentMatchesNameOnly(const std::string& index_string,
                                            const ParsedIdentifierComponent& comp) {
  const std::string& comp_name = comp.name();
  if (comp_name.size() > index_string.size())
    return false;  // Index string can't contain the name.

  if (strncmp(comp_name.c_str(), index_string.c_str(), comp_name.size()) != 0)
    return false;  // Name prefix doesn't match.

  // The index string should be at the end or should have a template spec
  // following the name.
  return index_string.size() == comp_name.size() || IsNameEnd(index_string[comp_name.size()]);
}

// static
bool IndexWalker2::ComponentMatchesTemplateOnly(const std::string& index_string,
                                                const ParsedIdentifierComponent& comp) {
  ParsedIdentifier index_ident;
  Err err = ExprParser::ParseIdentifier(index_string, &index_ident);
  if (err.has_error())
    return false;

  // Each namespaced component should be a different layer of the index so it should produce a
  // one-component identifier. But this depends how the symbols are structured which we don't want
  // to make assumptions about.
  if (index_ident.components().size() != 1)
    return false;
  const auto& index_comp = index_ident.components()[0];

  if (comp.has_template() != index_comp.has_template())
    return false;
  return comp.template_contents() == index_comp.template_contents();
}

// static
bool IndexWalker2::IsIndexStringBeyondName(std::string_view index_name, std::string_view name) {
  if (index_name.size() <= name.size()) {
    // The |index_name| is too small to start with the name and have template stuff on it (which
    // requires special handling), so we can directly return the answer by string comparison.
    return index_name > name;
  }

  // When the first name.size() characters of the index string aren't the same as the name, we don't
  // need to worry about templates or anything and can just return that comparison.
  std::string_view index_prefix = index_name.substr(0, name.size());
  int prefix_compare = index_prefix.compare(name);
  if (prefix_compare != 0)
    return prefix_compare > 0;  // Index is beyond the name by prefix only.

  // |index_name| starts with |name|. For the index node to be after all possible templates of
  // |name|, compare against the template begin character. This does make the assumption that the
  // compiler won't write templates with a space after the name ("vector < int >").
  return index_name[name.size()] > '<';
}

}  // namespace zxdb
