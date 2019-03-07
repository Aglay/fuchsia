// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <memory>
#include <vector>

#include "garnet/bin/zxdb/common/err.h"
#include "garnet/bin/zxdb/expr/expr_node.h"
#include "garnet/bin/zxdb/expr/expr_token.h"

namespace zxdb {

class ExprParser {
 public:
  ExprParser(std::vector<ExprToken> tokens);

  // Returns the root expression node on successful parsing. On error, returns
  // an empty pointer in which case the error message can be read from err()
  // ad error_token()
  fxl::RefPtr<ExprNode> Parse();

  // The result of parsing. Since this does not have access to the initial
  // string, it will not indicate context for the error. That can be generated
  // from the error_token() if desired.
  const Err& err() const { return err_; }

  ExprToken error_token() const { return error_token_; }

 private:
  struct DispatchInfo;

  // When recursively calling this function, call with the same precedence as
  // the current expression for left-associativity (operators evaluated from
  // left-to-right), and one less for right-associativity.
  fxl::RefPtr<ExprNode> ParseExpression(int precedence);

  // Parses template parameter lists. The "stop_before" parameter indicates how
  // the list is expected to end (i.e. ">"). Sets the error on failure.
  std::vector<std::string> ParseTemplateList(ExprToken::Type stop_before);

  // Parses comma-separated lists of expressions. Runs until the given ending
  // token is found (normally a ')' for a function call).
  std::vector<fxl::RefPtr<ExprNode>> ParseExpressionList(
      ExprToken::Type stop_before);

  fxl::RefPtr<ExprNode> AmpersandPrefix(const ExprToken& token);
  fxl::RefPtr<ExprNode> BinaryOpInfix(fxl::RefPtr<ExprNode> left,
                                      const ExprToken& token);
  fxl::RefPtr<ExprNode> ScopeInfix(fxl::RefPtr<ExprNode> left,
                                   const ExprToken& token);
  fxl::RefPtr<ExprNode> ScopePrefix(const ExprToken& token);
  fxl::RefPtr<ExprNode> DotOrArrowInfix(fxl::RefPtr<ExprNode> left,
                                        const ExprToken& token);
  fxl::RefPtr<ExprNode> LeftParenPrefix(const ExprToken& token);
  fxl::RefPtr<ExprNode> LeftParenInfix(fxl::RefPtr<ExprNode> left,
                                       const ExprToken& token);
  fxl::RefPtr<ExprNode> LeftSquareInfix(fxl::RefPtr<ExprNode> left,
                                        const ExprToken& token);
  fxl::RefPtr<ExprNode> LessInfix(fxl::RefPtr<ExprNode> left,
                                  const ExprToken& token);
  fxl::RefPtr<ExprNode> LiteralPrefix(const ExprToken& token);
  fxl::RefPtr<ExprNode> GreaterInfix(fxl::RefPtr<ExprNode> left,
                                     const ExprToken& token);
  fxl::RefPtr<ExprNode> MinusPrefix(const ExprToken& token);
  fxl::RefPtr<ExprNode> NamePrefix(const ExprToken& token);
  fxl::RefPtr<ExprNode> NameInfix(fxl::RefPtr<ExprNode> left,
                                  const ExprToken& token);
  fxl::RefPtr<ExprNode> StarPrefix(const ExprToken& token);

  // Returns true if the next token is the given type.
  bool LookAhead(ExprToken::Type type) const;

  // Returns the next token or the invalid token if nothing is left. Advances
  // to the next token.
  const ExprToken& Consume();

  // Consumes a token of the given type, returning it if there was one
  // available and the type matches. Otherwise, sets the error condition using
  // the given error_token and message, and returns a reference to an invalid
  // token. It will advance to the next token.
  const ExprToken& Consume(ExprToken::Type type, const ExprToken& error_token,
                           const char* error_msg);

  // Joins two IdentifierExprNodes. The |left| can be null which will prepend
  // the scope token to the right (generating a fully-qualified identifier).
  // Otherwise, right is null-checked and both are checked that they're
  // identifiers.
  fxl::RefPtr<ExprNode> JoinIdentifiers(fxl::RefPtr<ExprNode> left,
                                        const ExprToken& scope_token,
                                        fxl::RefPtr<ExprNode> right);

  void SetError(const ExprToken& token, std::string msg);

  // Call this only if !at_end().
  const ExprToken& cur_token() const { return tokens_[cur_]; }

  bool has_error() const { return err_.has_error(); }
  bool at_end() const { return cur_ == tokens_.size(); }

  static DispatchInfo kDispatchInfo[];

  std::vector<ExprToken> tokens_;
  size_t cur_ = 0;  // Current index into tokens_.

  // On error, the message and token where an error was encountered.
  Err err_;
  ExprToken error_token_;

  // This is a kInvalid token that we can return in error cases without having
  // to reference something in the tokens_ array.
  static const ExprToken kInvalidToken;
};

}  // namespace zxdb
