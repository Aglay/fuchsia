// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include "garnet/bin/zxdb/client/symbols/symbol.h"

namespace zxdb {

// Base class for anything that has code: lexical blocks, inlined subroutines,
// and functions. A DWARF lexical block is represented as a CodeBlock rather
// than a derived type since it has no additional attributes.
class CodeBlock : public Symbol {
 public:
  // Construct with fxl::MakeRefCounted().

  // A [begin, end) range of code blocks. These are addresses RELATIVE to the
  // beginning of the module they're inside of.
  using CodeRange = std::pair<uint64_t, uint64_t>;
  using CodeRanges = std::vector<CodeRange>;

  // Symbol overrides.
  const CodeBlock* AsCodeBlock() const override;

  // The enclosing symbol. This could be many things. For inlined subroutines
  // or lexical block, it could be an inlined subroutine, a lexical block,
  // or a function. For a function it could be a class, namespace, or
  // the top-level compilation unit.
  //
  // In the case of function implementations with separate definitions, the
  // decoder will set the enclosing symbol to be the enclosing scope around
  // the definition, which is how one will discover classes and namespaces
  // that the function is in. This is what callers normally want, but it means
  // that the enclosing symbol isn't necessarily the physical parent of the
  // DIE that generated this symbol.
  const LazySymbol& enclosing() const { return enclosing_; }
  void set_enclosing(const LazySymbol& e) { enclosing_ = e; }

  // The valid ranges of code for this block. In many cases there will be only
  // one range (most functions specify DW_AT_low_pc and DW_AT_high_pc), but
  // some blocks, especially inlined subroutines, may be at multiple
  // discontiguous ranges in the code (DW_AT_ranges are specified).
  //
  // Function declarations will have no ranges associated with them. These
  // arent't strictly "code blocks" but many functions won't have a
  // declaration/implementation split and there's so much overlap it's more
  // convenient to just have one type representing both.
  const CodeRanges& code_ranges() const { return code_ranges_; }
  void set_code_ranges(CodeRanges r) { code_ranges_ = std::move(r); }

  // The lexical blocks that are children of this one.
  const std::vector<LazySymbol>& inner_blocks() const { return inner_blocks_; }
  void set_inner_blocks(std::vector<LazySymbol> ib) {
    inner_blocks_ = std::move(ib);
  }

  // Variables contained within this block.
  const std::vector<LazySymbol>& variables() const { return variables_; }
  void set_variables(std::vector<LazySymbol> v) { variables_ = std::move(v); }

 protected:
  FRIEND_REF_COUNTED_THREAD_SAFE(CodeBlock);
  FRIEND_MAKE_REF_COUNTED(CodeBlock);

  explicit CodeBlock(int tag);
  ~CodeBlock() override;

 private:
  LazySymbol enclosing_;
  CodeRanges code_ranges_;
  std::vector<LazySymbol> inner_blocks_;
  std::vector<LazySymbol> variables_;
};

}  // namespace zxdb
