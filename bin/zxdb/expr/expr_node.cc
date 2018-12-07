// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/zxdb/expr/expr_node.h"

#include <stdlib.h>

#include <ostream>

#include "garnet/bin/zxdb/common/err.h"
#include "garnet/bin/zxdb/expr/expr_eval_context.h"
#include "garnet/bin/zxdb/expr/expr_value.h"
#include "garnet/bin/zxdb/expr/resolve_array.h"
#include "garnet/bin/zxdb/expr/resolve_collection.h"
#include "garnet/bin/zxdb/expr/resolve_ptr_ref.h"
#include "garnet/bin/zxdb/expr/symbol_variable_resolver.h"
#include "garnet/bin/zxdb/symbols/arch.h"
#include "garnet/bin/zxdb/symbols/array_type.h"
#include "garnet/bin/zxdb/symbols/base_type.h"
#include "garnet/bin/zxdb/symbols/data_member.h"
#include "garnet/bin/zxdb/symbols/modified_type.h"
#include "garnet/bin/zxdb/symbols/symbol_data_provider.h"
#include "lib/fxl/strings/string_printf.h"

namespace zxdb {

namespace {

std::string IndentFor(int value) { return std::string(value, ' '); }

bool BaseTypeCanBeArrayIndex(const BaseType* type) {
  int bt = type->base_type();
  return bt == BaseType::kBaseTypeBoolean || bt == BaseType::kBaseTypeSigned ||
         bt == BaseType::kBaseTypeSignedChar ||
         bt == BaseType::kBaseTypeUnsigned ||
         bt == BaseType::kBaseTypeUnsignedChar;
}

void EvalUnaryOperator(const ExprToken& op_token, const ExprValue& value,
                       ExprNode::EvalCallback cb) {
  // This manually extracts the value rather than calling PromoteToInt64() so
  // that the result type is exactly the same as the input type.
  //
  // TODO(brettw) when we add more mathematical operations we'll want a
  // more flexible system for getting the results out.
  if (op_token.type() == ExprToken::kMinus) {
    // Currently "-" is the only unary operator.  Since this is a debugger
    // primarily for C-like languages, use the C rules for negating values: the
    // result type is the same as the input, and negating an unsigned value
    // gives the two's compliment (C++11 standard section 5.3.1).
    switch (value.GetBaseType()) {
      case BaseType::kBaseTypeSigned:
        switch (value.data().size()) {
          case sizeof(int8_t):
            cb(Err(), ExprValue(-value.GetAs<int8_t>()));
            return;
          case sizeof(int16_t):
            cb(Err(), ExprValue(-value.GetAs<int16_t>()));
            return;
          case sizeof(int32_t):
            cb(Err(), ExprValue(-value.GetAs<int32_t>()));
            return;
          case sizeof(int64_t):
            cb(Err(), ExprValue(-value.GetAs<int64_t>()));
            return;
        }
        break;

      case BaseType::kBaseTypeUnsigned:
        switch (value.data().size()) {
          case sizeof(uint8_t):
            cb(Err(), ExprValue(-value.GetAs<uint8_t>()));
            return;
          case sizeof(uint16_t):
            cb(Err(), ExprValue(-value.GetAs<uint16_t>()));
            return;
          case sizeof(uint32_t):
            cb(Err(), ExprValue(-value.GetAs<uint32_t>()));
            return;
          case sizeof(uint64_t):
            cb(Err(), ExprValue(-value.GetAs<uint64_t>()));
            return;
        }
        break;

      default:
        FXL_NOTREACHED();
    }
    cb(Err("Negation for this value is not supported."), ExprValue());
    return;
  }
  FXL_NOTREACHED();
  cb(Err("Internal error evaluating unary operator."), ExprValue());
}

}  // namespace

void ExprNode::EvalFollowReferences(fxl::RefPtr<ExprEvalContext> context,
                                    EvalCallback cb) const {
  Eval(context,
       [ context, cb = std::move(cb) ](const Err& err, ExprValue value) {
         if (err.has_error()) {
           cb(err, ExprValue());
         } else {
           EnsureResolveReference(context->GetDataProvider(), std::move(value),
                                  std::move(cb));
         }
       });
}

void AddressOfExprNode::Eval(fxl::RefPtr<ExprEvalContext> context,
                             EvalCallback cb) const {
  expr_->EvalFollowReferences(
      context, [cb = std::move(cb)](const Err& err, ExprValue value) {
        if (value.source().type() != ExprValueSource::Type::kMemory) {
          cb(Err("Can't take the address of a temporary."), ExprValue());
        } else {
          // Construct a pointer type to the variable.
          auto ptr_type = fxl::MakeRefCounted<ModifiedType>(
              Symbol::kTagPointerType, LazySymbol(value.type_ref()));

          std::vector<uint8_t> contents;
          contents.resize(kTargetPointerSize);
          TargetPointer address = value.source().address();
          memcpy(&contents[0], &address, sizeof(kTargetPointerSize));

          cb(Err(), ExprValue(std::move(ptr_type), std::move(contents)));
        }
      });
}

void AddressOfExprNode::Print(std::ostream& out, int indent) const {
  out << IndentFor(indent) << "ADDRESS_OF\n";
  expr_->Print(out, indent + 1);
}

void ArrayAccessExprNode::Eval(fxl::RefPtr<ExprEvalContext> context,
                               EvalCallback cb) const {
  left_->EvalFollowReferences(
      context, [ inner = inner_, context, cb = std::move(cb) ](
                   const Err& err, ExprValue left_value) {
        if (err.has_error()) {
          cb(err, ExprValue());
        } else {
          // "left" has been evaluated, now do "inner".
          inner->EvalFollowReferences(context, [
            context, left_value = std::move(left_value), cb = std::move(cb)
          ](const Err& err, ExprValue inner_value) {
            if (err.has_error()) {
              cb(err, ExprValue());
            } else {
              // Both "left" and "inner" has been evaluated.
              int64_t offset = 0;
              Err offset_err = InnerValueToOffset(inner_value, &offset);
              if (offset_err.has_error()) {
                cb(offset_err, ExprValue());
              } else {
                DoAccess(std::move(context), std::move(left_value), offset,
                         std::move(cb));
              }
            }
          });
        }
      });
}

// static
Err ArrayAccessExprNode::InnerValueToOffset(const ExprValue& inner,
                                            int64_t* offset) {
  // Type should be some kind of number.
  const Type* type = inner.type();
  if (!type)
    return Err("Bad type, please file a bug with a repro.");
  type = type->GetConcreteType();  // Skip "const", etc.

  const BaseType* base_type = type->AsBaseType();
  if (!base_type || !BaseTypeCanBeArrayIndex(base_type))
    return Err("Bad type for array index.");

  // This uses signed integers to explicitly allow negative indexing which the
  // user may want to do for some reason.
  Err promote_err = inner.PromoteToInt64(offset);
  if (promote_err.has_error())
    return promote_err;
  return Err();
}

// static
void ArrayAccessExprNode::DoAccess(fxl::RefPtr<ExprEvalContext> context,
                                   ExprValue left, int64_t offset,
                                   EvalCallback cb) {
  ResolveArray(
      context->GetDataProvider(), left, static_cast<size_t>(offset),
      static_cast<size_t>(offset) + 1,
      [cb = std::move(cb)](const Err& err, std::vector<ExprValue> result) {
        if (err.has_error()) {
          cb(err, ExprValue());
          return;
        }
        if (result.size() == 0) {
          // Short read, array not big enough.
          cb(Err("Array index out of range."), ExprValue());
          return;
        }
        FXL_DCHECK(result.size() == 1);
        cb(Err(), result[0]);
      });
}

void ArrayAccessExprNode::Print(std::ostream& out, int indent) const {
  out << IndentFor(indent) << "ARRAY_ACCESS\n";
  left_->Print(out, indent + 1);
  inner_->Print(out, indent + 1);
}

void DereferenceExprNode::Eval(fxl::RefPtr<ExprEvalContext> context,
                               EvalCallback cb) const {
  expr_->EvalFollowReferences(context, [ context, cb = std::move(cb) ](
                                           const Err& err, ExprValue value) {
    ResolvePointer(context->GetDataProvider(), value, std::move(cb));
  });
}

void DereferenceExprNode::Print(std::ostream& out, int indent) const {
  out << IndentFor(indent) << "DEREFERENCE\n";
  expr_->Print(out, indent + 1);
}

void IdentifierExprNode::Eval(fxl::RefPtr<ExprEvalContext> context,
                              EvalCallback cb) const {
  // For now, pass the stringified identifier. In the future we'll want to pass
  // the Identifier itself so the namespaces can be resolved.
  context->GetNamedValue(
      ident_, [cb = std::move(cb)](const Err& err, fxl::RefPtr<Symbol>,
                                   ExprValue value) {
        // Discard resolved symbol, we only need the value.
        cb(err, std::move(value));
      });
}

void IdentifierExprNode::Print(std::ostream& out, int indent) const {
  out << IndentFor(indent) << "IDENTIFIER(" << ident_.GetDebugName() << ")\n";
}

void IntegerExprNode::Eval(fxl::RefPtr<ExprEvalContext> context,
                           EvalCallback cb) const {
  // The tokenizer will have already validated the integer format.
  cb(Err(), ExprValue(static_cast<int64_t>(atoll(integer_.value().c_str()))));
}

void IntegerExprNode::Print(std::ostream& out, int indent) const {
  out << IndentFor(indent) << "INTEGER(" << integer_.value() << ")\n";
}

void MemberAccessExprNode::Eval(fxl::RefPtr<ExprEvalContext> context,
                                EvalCallback cb) const {
  bool is_arrow = accessor_.type() == ExprToken::kArrow;
  left_->EvalFollowReferences(context, [
    context, is_arrow, member = member_, cb = std::move(cb)
  ](const Err& err, ExprValue base) {
    if (!is_arrow) {
      // "." operator.
      ExprValue result;
      Err err = ResolveMember(base, member, &result);
      cb(err, std::move(result));
      return;
    }

    // Everything else should be a -> operator.
    ResolveMemberByPointer(
        context, base, member,
        [cb = std::move(cb)](const Err& err, fxl::RefPtr<Symbol>,
                             ExprValue result) {
          // Discard resolved symbol, we only need the value.
          cb(err, std::move(result));
        });
  });
}

void MemberAccessExprNode::Print(std::ostream& out, int indent) const {
  out << IndentFor(indent) << "ACCESSOR(" << accessor_.value() << ")\n";
  left_->Print(out, indent + 1);
  out << IndentFor(indent + 1) << member_.GetFullName() << "\n";
}

void UnaryOpExprNode::Eval(fxl::RefPtr<ExprEvalContext> context,
                           EvalCallback cb) const {
  expr_->EvalFollowReferences(context, [ cb = std::move(cb), op = op_ ](
                                           const Err& err, ExprValue value) {
    if (err.has_error())
      cb(err, std::move(value));
    else
      EvalUnaryOperator(op, value, std::move(cb));
  });
}

void UnaryOpExprNode::Print(std::ostream& out, int indent) const {
  out << IndentFor(indent) << "UNARY(" << op_.value() << ")\n";
  expr_->Print(out, indent + 1);
}

}  // namespace zxdb
