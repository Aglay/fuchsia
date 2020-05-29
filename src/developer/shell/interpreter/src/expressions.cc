// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/shell/interpreter/src/expressions.h"

#include <ostream>

#include "src/developer/shell/interpreter/src/instructions.h"
#include "src/developer/shell/interpreter/src/interpreter.h"
#include "src/developer/shell/interpreter/src/nodes.h"
#include "src/developer/shell/interpreter/src/schema.h"
#include "src/developer/shell/interpreter/src/scope.h"
#include "src/developer/shell/interpreter/src/types.h"

namespace shell {
namespace interpreter {

// - IntegerLiteral --------------------------------------------------------------------------------

void IntegerLiteral::Dump(std::ostream& os) const {
  if (negative_) {
    os << '-';
  }
  os << absolute_value_;
}

bool IntegerLiteral::Compile(ExecutionContext* context, code::Code* code,
                             const Type* for_type) const {
  return for_type->GenerateIntegerLiteral(context, code, this);
}

// - ObjectField -----------------------------------------------------------------------------------

void ObjectDeclarationField::Dump(std::ostream& os) const {
  os << field_schema_->name() << ": " << *(field_schema_->type()) << " = " << *expression_;
}

// - ObjectDeclaration -----------------------------------------------------------------------------

ObjectDeclaration::ObjectDeclaration(Interpreter* interpreter, uint64_t file_id, uint64_t node_id,
                                     std::shared_ptr<ObjectSchema> object_schema,
                                     std::vector<std::unique_ptr<ObjectDeclarationField>>&& fields)
    : Expression(interpreter, file_id, node_id),
      object_schema_(object_schema),
      fields_(std::move(fields)) {
  // Fields need to be in the same order that they are in the schema.  Find them in the schema and
  // insert them at the correct point.
  for (size_t i = 0; i < object_schema->fields().size(); i++) {
    if (object_schema->fields()[i].get() != fields_[i]->schema()) {
      bool found = false;
      for (size_t j = i + 1; j < object_schema->fields().size(); j++) {
        if (object_schema->fields()[i].get() == fields_[j]->schema()) {
          ObjectDeclarationField* tmp = fields_[j].release();
          fields_[j].reset(fields_[i].release());
          fields_[i].reset(tmp);
          found = true;
          break;
        }
      }
      FX_DCHECK(found) << "Unable to find schema for field";
    }
  }
}

void ObjectDeclaration::Dump(std::ostream& os) const {
  os << "{";
  const char* separator = "";
  for (size_t i = 0; i < fields_.size(); i++) {
    os << separator << *(fields_[i]);
    separator = ", ";
  }
  os << "}";
}

bool ObjectDeclaration::Compile(ExecutionContext* context, code::Code* code,
                                const Type* for_type) const {
  const TypeObject* object_type = const_cast<Type*>(for_type)->AsTypeObject();
  object_type->GenerateInitialization(context, code, this);
  object_type->GenerateObject(context, code, this);
  return true;
}

// - StringLiteral ---------------------------------------------------------------------------------

void StringLiteral::Dump(std::ostream& os) const {
  // TODO(vbelliard): escape special characters.
  os << '"' << string()->value() << '"';
}

bool StringLiteral::Compile(ExecutionContext* context, code::Code* code,
                            const Type* for_type) const {
  return for_type->GenerateStringLiteral(context, code, this);
}

// - ExpressionVariable ----------------------------------------------------------------------------

void ExpressionVariable::Dump(std::ostream& os) const { os << variable_definition_.StringId(); }

bool ExpressionVariable::Compile(ExecutionContext* context, code::Code* code,
                                 const Type* for_type) const {
  const Variable* definition = context->interpreter()->SearchGlobal(variable_definition_);
  if (definition == nullptr) {
    context->EmitError(id(), "Can't find variable " + variable_definition_.StringId() + ".");
    return false;
  }
  return for_type->GenerateVariable(context, code, id(), definition);
}

// - Addition --------------------------------------------------------------------------------------

void Addition::Dump(std::ostream& os) const {
  os << *left() << (with_exceptions_ ? " +? " : " + ") << *right();
}

bool Addition::Compile(ExecutionContext* context, code::Code* code, const Type* for_type) const {
  return for_type->GenerateAddition(context, code, this);
}

size_t Addition::GenerateStringTerms(ExecutionContext* context, code::Code* code,
                                     const Type* for_type) const {
  return left()->GenerateStringTerms(context, code, for_type) +
         right()->GenerateStringTerms(context, code, for_type);
}

}  // namespace interpreter
}  // namespace shell
