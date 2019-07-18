// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_EXPR_PRETTY_VECTOR_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_EXPR_PRETTY_VECTOR_H_

#include "src/developer/debug/zxdb/expr/pretty_type.h"

namespace zxdb {

// C++ std::vector.
class PrettyStdVector : public PrettyType {
 public:
  PrettyStdVector() = default;

  // PrettyType implementation.
  void Format(FormatNode* node, const FormatOptions& options, fxl::RefPtr<EvalContext> context,
              fit::deferred_callback cb) override;
};

// Rust vec::Vec.
class PrettyRustVec : public PrettyType {
 public:
  PrettyRustVec() = default;

  // PrettyType implementation.
  void Format(FormatNode* node, const FormatOptions& options, fxl::RefPtr<EvalContext> context,
              fit::deferred_callback cb) override;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_EXPR_PRETTY_VECTOR_H_
