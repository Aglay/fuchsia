// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fidl/json_generator.h"

#include "fidl/names.h"

namespace fidl {

void JSONGenerator::Generate(const flat::Decl* decl) { Generate(decl->name); }

void JSONGenerator::Generate(SourceSpan value) { EmitString(value.data()); }

void JSONGenerator::Generate(NameSpan value) {
  GenerateObject([&]() {
    GenerateObjectMember("filename", value.filename, Position::kFirst);
    GenerateObjectMember("line", (uint32_t)value.position.line);
    GenerateObjectMember("column", (uint32_t)value.position.column);
  });
}

void JSONGenerator::Generate(const flat::ConstantValue& value) {
  switch (value.kind) {
    case flat::ConstantValue::Kind::kUint8: {
      auto numeric_constant = reinterpret_cast<const flat::NumericConstantValue<uint8_t>&>(value);
      EmitNumeric(static_cast<uint64_t>(numeric_constant), kAsString);
      break;
    }
    case flat::ConstantValue::Kind::kUint16: {
      auto numeric_constant = reinterpret_cast<const flat::NumericConstantValue<uint16_t>&>(value);
      EmitNumeric(static_cast<uint16_t>(numeric_constant), kAsString);
      break;
    }
    case flat::ConstantValue::Kind::kUint32: {
      auto numeric_constant = reinterpret_cast<const flat::NumericConstantValue<uint32_t>&>(value);
      EmitNumeric(static_cast<uint32_t>(numeric_constant), kAsString);
      break;
    }
    case flat::ConstantValue::Kind::kUint64: {
      auto numeric_constant = reinterpret_cast<const flat::NumericConstantValue<uint64_t>&>(value);
      EmitNumeric(static_cast<uint64_t>(numeric_constant), kAsString);
      break;
    }
    case flat::ConstantValue::Kind::kInt8: {
      auto numeric_constant = reinterpret_cast<const flat::NumericConstantValue<int8_t>&>(value);
      EmitNumeric(static_cast<int64_t>(numeric_constant), kAsString);
      break;
    }
    case flat::ConstantValue::Kind::kInt16: {
      auto numeric_constant = reinterpret_cast<const flat::NumericConstantValue<int16_t>&>(value);
      EmitNumeric(static_cast<int16_t>(numeric_constant), kAsString);
      break;
    }
    case flat::ConstantValue::Kind::kInt32: {
      auto numeric_constant = reinterpret_cast<const flat::NumericConstantValue<int32_t>&>(value);
      EmitNumeric(static_cast<int32_t>(numeric_constant), kAsString);
      break;
    }
    case flat::ConstantValue::Kind::kInt64: {
      auto numeric_constant = reinterpret_cast<const flat::NumericConstantValue<int64_t>&>(value);
      EmitNumeric(static_cast<int64_t>(numeric_constant), kAsString);
      break;
    }
    case flat::ConstantValue::Kind::kFloat32: {
      auto numeric_constant = reinterpret_cast<const flat::NumericConstantValue<float>&>(value);
      EmitNumeric(static_cast<float>(numeric_constant), kAsString);
      break;
    }
    case flat::ConstantValue::Kind::kFloat64: {
      auto numeric_constant = reinterpret_cast<const flat::NumericConstantValue<double>&>(value);
      EmitNumeric(static_cast<double>(numeric_constant), kAsString);
      break;
    }
    case flat::ConstantValue::Kind::kBool: {
      auto bool_constant = reinterpret_cast<const flat::BoolConstantValue&>(value);
      EmitBoolean(static_cast<bool>(bool_constant), kAsString);
      break;
    }
    case flat::ConstantValue::Kind::kString: {
      auto string_constant = reinterpret_cast<const flat::StringConstantValue&>(value);
      EmitLiteral(string_constant.value);
      break;
    }
  }  // switch
}

void JSONGenerator::Generate(types::HandleSubtype value) { EmitString(NameHandleSubtype(value)); }

void JSONGenerator::Generate(types::Nullability value) {
  switch (value) {
    case types::Nullability::kNullable:
      EmitBoolean(true);
      break;
    case types::Nullability::kNonnullable:
      EmitBoolean(false);
      break;
  }
}

void JSONGenerator::Generate(const raw::Identifier& value) { EmitString(value.span().data()); }

void JSONGenerator::Generate(const flat::LiteralConstant& value) {
  GenerateObject([&]() {
    GenerateObjectMember("kind", NameRawLiteralKind(value.literal->kind), Position::kFirst);

    // TODO(FIDL-486): Since some constants are not properly resolved during
    // library compilation, we must be careful in emitting the resolved
    // value. Currently, we fall back using the original value, despite this
    // being problematic in the case of binary literals.
    if (value.IsResolved()) {
      GenerateObjectMember("value", value.Value());
    } else {
      switch (value.literal->kind) {
        case raw::Literal::Kind::kString: {
          auto string_literal = static_cast<const raw::StringLiteral*>(value.literal.get());
          EmitObjectSeparator();
          EmitObjectKey("value");
          EmitLiteral(string_literal->span().data());
          break;
        }
        case raw::Literal::Kind::kNumeric:
        case raw::Literal::Kind::kTrue:
        case raw::Literal::Kind::kFalse:
          GenerateObjectMember("value", value.literal->span().data());
          break;
      }  // switch
    }
    GenerateObjectMember("expression", value.literal->span().data());
  });
}

void JSONGenerator::Generate(const flat::Constant& value) {
  GenerateObject([&]() {
    switch (value.kind) {
      case flat::Constant::Kind::kIdentifier: {
        GenerateObjectMember("kind", NameFlatConstantKind(value.kind), Position::kFirst);
        auto type = static_cast<const flat::IdentifierConstant*>(&value);
        GenerateObjectMember("identifier", type->name);
        break;
      }
      case flat::Constant::Kind::kLiteral: {
        GenerateObjectMember("kind", NameFlatConstantKind(value.kind), Position::kFirst);
        auto& type = static_cast<const flat::LiteralConstant&>(value);
        GenerateObjectMember("literal", type);
        break;
      }
      case flat::Constant::Kind::kSynthesized: {
        // TODO(pascallouis): We should explore exposing these in the JSON IR, such that the
        // implicit bounds are made explicit by fidlc, rather than sprinkled throughout all
        // backends.
        //
        // For now, do not emit synthesized constants
        break;
      }
    }
  });
}

void JSONGenerator::Generate(const flat::Type* value) {
  GenerateObject([&]() {
    GenerateObjectMember("kind", NameFlatTypeKind(value->kind), Position::kFirst);

    switch (value->kind) {
      case flat::Type::Kind::kArray: {
        auto type = static_cast<const flat::ArrayType*>(value);
        GenerateObjectMember("element_type", type->element_type);
        GenerateObjectMember("element_count", type->element_count->value);
        break;
      }
      case flat::Type::Kind::kVector: {
        auto type = static_cast<const flat::VectorType*>(value);
        GenerateObjectMember("element_type", type->element_type);
        if (*type->element_count < flat::Size::Max())
          GenerateObjectMember("maybe_element_count", type->element_count->value);
        GenerateObjectMember("nullable", type->nullability);
        break;
      }
      case flat::Type::Kind::kString: {
        auto type = static_cast<const flat::StringType*>(value);
        if (*type->max_size < flat::Size::Max())
          GenerateObjectMember("maybe_element_count", type->max_size->value);
        GenerateObjectMember("nullable", type->nullability);
        break;
      }
      case flat::Type::Kind::kHandle: {
        auto type = static_cast<const flat::HandleType*>(value);
        GenerateObjectMember("subtype", type->subtype);
        GenerateObjectMember("nullable", type->nullability);
        break;
      }
      case flat::Type::Kind::kRequestHandle: {
        auto type = static_cast<const flat::RequestHandleType*>(value);
        GenerateObjectMember("subtype", type->protocol_type->name);
        GenerateObjectMember("nullable", type->nullability);
        break;
      }
      case flat::Type::Kind::kPrimitive: {
        auto type = static_cast<const flat::PrimitiveType*>(value);
        GenerateObjectMember("subtype", type->name);
        break;
      }
      case flat::Type::Kind::kIdentifier: {
        auto type = static_cast<const flat::IdentifierType*>(value);
        GenerateObjectMember("identifier", type->name);
        GenerateObjectMember("nullable", type->nullability);
        break;
      }
    }
  });
}

void JSONGenerator::Generate(const raw::Attribute& value) {
  GenerateObject([&]() {
    GenerateObjectMember("name", value.name, Position::kFirst);
    if (value.value != "")
      GenerateObjectMember("value", value.value);
    else
      GenerateObjectMember("value", std::string_view());
  });
}

void JSONGenerator::Generate(const raw::AttributeList& value) { Generate(value.attributes); }

void JSONGenerator::Generate(const raw::Ordinal32& value) { EmitNumeric(value.value); }

void JSONGenerator::Generate(const raw::Ordinal64& value) { EmitNumeric(value.value); }

void JSONGenerator::Generate(const flat::Name& value) {
  // These look like (when there is a library)
  //     { "LIB.LIB.LIB", "ID" }
  // or (when there is not)
  //     { "ID" }
  Generate(NameFlatName(value));
}

void JSONGenerator::Generate(const flat::Bits& value) {
  GenerateObject([&]() {
    GenerateObjectMember("name", value.name, Position::kFirst);
    GenerateObjectMember("location", NameSpan(value.name));
    if (value.attributes)
      GenerateObjectMember("maybe_attributes", value.attributes);
    GenerateTypeAndFromTypeAlias(*value.subtype_ctor);
    // TODO(FIDL-324): When all numbers are wrapped as string, we can simply
    // call GenerateObjectMember directly.
    GenerateObjectPunctuation(Position::kSubsequent);
    EmitObjectKey("mask");
    EmitNumeric(value.mask, kAsString);
    GenerateObjectMember("members", value.members);
    GenerateObjectMember("strict", value.strictness == types::Strictness::kStrict);
  });
}

void JSONGenerator::Generate(const flat::Bits::Member& value) {
  GenerateObject([&]() {
    GenerateObjectMember("name", value.name, Position::kFirst);
    GenerateObjectMember("location", NameSpan(value.name));
    GenerateObjectMember("value", value.value);
    if (value.attributes)
      GenerateObjectMember("maybe_attributes", value.attributes);
  });
}

void JSONGenerator::Generate(const flat::Const& value) {
  GenerateObject([&]() {
    GenerateObjectMember("name", value.name, Position::kFirst);
    GenerateObjectMember("location", NameSpan(value.name));
    if (value.attributes)
      GenerateObjectMember("maybe_attributes", value.attributes);
    GenerateTypeAndFromTypeAlias(*value.type_ctor);
    GenerateObjectMember("value", value.value);
  });
}

void JSONGenerator::Generate(const flat::Enum& value) {
  GenerateObject([&]() {
    GenerateObjectMember("name", value.name, Position::kFirst);
    GenerateObjectMember("location", NameSpan(value.name));
    if (value.attributes)
      GenerateObjectMember("maybe_attributes", value.attributes);
    // TODO(FIDL-324): Due to legacy reasons, the 'type' of enums is actually
    // the primitive subtype, and therefore cannot use
    // GenerateTypeAndFromTypeAlias here.
    GenerateObjectMember("type", value.type->name);
    if (value.subtype_ctor->from_type_alias)
      GenerateObjectMember("experimental_maybe_from_type_alias",
                           value.subtype_ctor->from_type_alias.value());
    GenerateObjectMember("members", value.members);
    GenerateObjectMember("strict", value.strictness == types::Strictness::kStrict);
  });
}

void JSONGenerator::Generate(const flat::Enum::Member& value) {
  GenerateObject([&]() {
    GenerateObjectMember("name", value.name, Position::kFirst);
    GenerateObjectMember("location", NameSpan(value.name));
    GenerateObjectMember("value", value.value);
    if (value.attributes)
      GenerateObjectMember("maybe_attributes", value.attributes);
  });
}

void JSONGenerator::Generate(const flat::Protocol& value) {
  GenerateObject([&]() {
    GenerateObjectMember("name", value.name, Position::kFirst);
    GenerateObjectMember("location", NameSpan(value.name));
    if (value.attributes)
      GenerateObjectMember("maybe_attributes", value.attributes);
    GenerateObjectMember("methods", value.all_methods);
  });
}

void JSONGenerator::Generate(const flat::Protocol::MethodWithInfo& method_with_info) {
  assert(method_with_info.method != nullptr);
  const auto& value = *method_with_info.method;
  GenerateObject([&]() {
    GenerateObjectPunctuation(Position::kFirst);
    EmitObjectKey("ordinal");
    EmitNumeric(static_cast<uint64_t>(value.generated_ordinal32->value) << 32);
    GenerateObjectPunctuation(Position::kSubsequent);
    EmitObjectKey("generated_ordinal");
    EmitNumeric(static_cast<uint64_t>(value.generated_ordinal64->value));
    GenerateObjectMember("name", value.name);
    GenerateObjectMember("location", NameSpan(value.name));
    GenerateObjectMember("has_request", value.maybe_request != nullptr);
    if (value.attributes)
      GenerateObjectMember("maybe_attributes", value.attributes);
    if (value.maybe_request != nullptr) {
      GenerateRequest("maybe_request", *value.maybe_request);
    }
    GenerateObjectMember("has_response", value.maybe_response != nullptr);
    if (value.maybe_response != nullptr) {
      GenerateRequest("maybe_response", *value.maybe_response);
    }
    GenerateObjectMember("is_composed", method_with_info.is_composed);
  });
}

void JSONGenerator::GenerateTypeAndFromTypeAlias(const flat::TypeConstructor& value,
                                                 Position position) {
  GenerateObjectMember("type", value.type, position);
  if (value.from_type_alias)
    GenerateObjectMember("experimental_maybe_from_type_alias", value.from_type_alias.value());
}

void JSONGenerator::GenerateRequest(const std::string& prefix, const flat::Struct& value) {
  GenerateObjectMember(prefix, value.members);
  auto deprecated_type_shape = value.typeshape(WireFormat::kOld);
  GenerateObjectMember(prefix + "_size", deprecated_type_shape.InlineSize());
  GenerateObjectMember(prefix + "_alignment", deprecated_type_shape.Alignment());
  GenerateObjectMember(prefix + "_has_padding", deprecated_type_shape.HasPadding());
  GenerateObjectMember("experimental_" + prefix + "_has_flexible_envelope",
                       deprecated_type_shape.HasFlexibleEnvelope());
  GenerateTypeShapes(prefix, value);
}

void JSONGenerator::Generate(const flat::Service& value) {
  GenerateObject([&]() {
    GenerateObjectMember("name", value.name, Position::kFirst);
    GenerateObjectMember("location", NameSpan(value.name));
    if (value.attributes)
      GenerateObjectMember("maybe_attributes", value.attributes);
    GenerateObjectMember("members", value.members);
  });
}

void JSONGenerator::Generate(const flat::Service::Member& value) {
  GenerateObject([&]() {
    GenerateTypeAndFromTypeAlias(*value.type_ctor, Position::kFirst);
    GenerateObjectMember("name", value.name);
    GenerateObjectMember("location", NameSpan(value.name));
    if (value.attributes)
      GenerateObjectMember("maybe_attributes", value.attributes);
  });
}

void JSONGenerator::Generate(const flat::Struct& value) {
  GenerateObject([&]() {
    GenerateObjectMember("name", value.name, Position::kFirst);
    GenerateObjectMember("location", NameSpan(value.name));
    GenerateObjectMember("anonymous", value.is_request_or_response);
    if (value.attributes)
      GenerateObjectMember("maybe_attributes", value.attributes);
    GenerateObjectMember("members", value.members);
    auto deprecated_type_shape = value.typeshape(WireFormat::kOld);
    GenerateObjectMember("size", deprecated_type_shape.InlineSize());
    GenerateObjectMember("max_out_of_line", deprecated_type_shape.MaxOutOfLine());
    GenerateObjectMember("alignment", deprecated_type_shape.Alignment());
    GenerateObjectMember("max_handles", deprecated_type_shape.MaxHandles());
    GenerateObjectMember("has_padding", deprecated_type_shape.HasPadding());
    GenerateTypeShapes(value);
  });
}

void JSONGenerator::Generate(const flat::Struct::Member& value) {
  GenerateObject([&]() {
    GenerateTypeAndFromTypeAlias(*value.type_ctor, Position::kFirst);
    GenerateObjectMember("name", value.name);
    GenerateObjectMember("location", NameSpan(value.name));
    if (value.attributes)
      GenerateObjectMember("maybe_attributes", value.attributes);
    if (value.maybe_default_value)
      GenerateObjectMember("maybe_default_value", value.maybe_default_value);
    auto deprecated_type_shape = value.typeshape(WireFormat::kOld);
    auto deprecated_field_shape = value.fieldshape(WireFormat::kOld);
    GenerateObjectMember("size", deprecated_type_shape.InlineSize());
    GenerateObjectMember("max_out_of_line", deprecated_type_shape.MaxOutOfLine());
    GenerateObjectMember("alignment", deprecated_type_shape.Alignment());
    GenerateObjectMember("offset", deprecated_field_shape.Offset());
    GenerateObjectMember("max_handles", deprecated_type_shape.MaxHandles());
    GenerateFieldShapes(value);
  });
}

void JSONGenerator::Generate(const flat::Table& value) {
  GenerateObject([&]() {
    GenerateObjectMember("name", value.name, Position::kFirst);
    GenerateObjectMember("location", NameSpan(value.name));
    if (value.attributes)
      GenerateObjectMember("maybe_attributes", value.attributes);
    GenerateObjectMember("members", value.members);
    auto deprecated_type_shape = value.typeshape(WireFormat::kOld);
    GenerateObjectMember("size", deprecated_type_shape.InlineSize());
    GenerateObjectMember("max_out_of_line", deprecated_type_shape.MaxOutOfLine());
    GenerateObjectMember("alignment", deprecated_type_shape.Alignment());
    GenerateObjectMember("max_handles", deprecated_type_shape.MaxHandles());
    GenerateObjectMember("strict", value.strictness == types::Strictness::kStrict);
    GenerateTypeShapes(value);
  });
}

void JSONGenerator::Generate(const flat::Table::Member& value) {
  GenerateObject([&]() {
    GenerateObjectMember("ordinal", *value.ordinal, Position::kFirst);
    if (value.maybe_used) {
      assert(!value.span);
      GenerateObjectMember("reserved", false);
      GenerateTypeAndFromTypeAlias(*value.maybe_used->type_ctor);
      GenerateObjectMember("name", value.maybe_used->name);
      GenerateObjectMember("location", NameSpan(value.maybe_used->name));
      if (value.maybe_used->attributes)
        GenerateObjectMember("maybe_attributes", value.maybe_used->attributes);
      // TODO(FIDL-609): Support defaults on tables.
      auto deprecated_type_shape = value.typeshape(WireFormat::kOld);
      GenerateObjectMember("size", deprecated_type_shape.InlineSize());
      GenerateObjectMember("max_out_of_line", deprecated_type_shape.MaxOutOfLine());
      GenerateObjectMember("alignment", deprecated_type_shape.Alignment());
      GenerateObjectMember("max_handles", deprecated_type_shape.MaxHandles());
    } else {
      assert(value.span);
      GenerateObjectMember("reserved", true);
      GenerateObjectMember("location", NameSpan(value.span.value()));
    }
  });
}

void JSONGenerator::Generate(const TypeShape& type_shape) {
  GenerateObject([&]() {
    GenerateObjectMember("inline_size", type_shape.inline_size, Position::kFirst);
    GenerateObjectMember("alignment", type_shape.alignment);
    GenerateObjectMember("depth", type_shape.depth);
    GenerateObjectMember("max_handles", type_shape.max_handles);
    GenerateObjectMember("max_out_of_line", type_shape.max_out_of_line);
    GenerateObjectMember("has_padding", type_shape.has_padding);
    GenerateObjectMember("has_flexible_envelope", type_shape.has_flexible_envelope);
    GenerateObjectMember("contains_union", type_shape.contains_union);
  });
}

void JSONGenerator::Generate(const FieldShape& field_shape) {
  GenerateObject([&]() {
    GenerateObjectMember("offset", field_shape.offset, Position::kFirst);
    GenerateObjectMember("padding", field_shape.padding);
  });
}

void JSONGenerator::Generate(const flat::Union& value) {
  GenerateObject([&]() {
    GenerateObjectMember("name", value.name, Position::kFirst);
    GenerateObjectMember("location", NameSpan(value.name));
    if (value.attributes)
      GenerateObjectMember("maybe_attributes", value.attributes);

    // As part of the union-to-xunion migration, static unions now use an
    // explicit syntax to specify their xunion_ordinal:
    //
    //     union Foo {
    //         1: int bar;  // union tag 0, xunion ordinal 1
    //         2: bool baz; // union tag 1, xunion ordinal 2
    //     };
    //
    // This makes it look like the variants can be safely reordered, like table
    // fields. However, since union tag indices come from the JSON members array
    // -- which usually follows source order -- it would break ABI. We prevent
    // this by sorting members by xunion_ordinal before emitting them.
    GenerateObjectMember("members", value.MembersSortedByXUnionOrdinal());

    auto deprecated_type_shape = value.typeshape(WireFormat::kOld);
    GenerateObjectMember("size", deprecated_type_shape.InlineSize());
    GenerateObjectMember("max_out_of_line", deprecated_type_shape.MaxOutOfLine());
    GenerateObjectMember("alignment", deprecated_type_shape.Alignment());
    GenerateObjectMember("max_handles", deprecated_type_shape.MaxHandles());
    GenerateTypeShapes(value);
  });
}

void JSONGenerator::Generate(const flat::Union::Member& value) {
  GenerateObject([&]() {
    GenerateObjectMember("xunion_ordinal", value.xunion_ordinal, Position::kFirst);
    if (value.maybe_used) {
      assert(!value.span);
      GenerateObjectMember("reserved", false);
      GenerateObjectMember("name", value.maybe_used->name);
      GenerateTypeAndFromTypeAlias(*value.maybe_used->type_ctor);
      GenerateObjectMember("location", NameSpan(value.maybe_used->name));
      if (value.maybe_used->attributes)
        GenerateObjectMember("maybe_attributes", value.maybe_used->attributes);
      auto deprecated_type_shape = value.maybe_used->typeshape(WireFormat::kOld);
      auto deprecated_field_shape = value.maybe_used->fieldshape(WireFormat::kOld);
      GenerateObjectMember("size", deprecated_type_shape.InlineSize());
      GenerateObjectMember("max_out_of_line", deprecated_type_shape.MaxOutOfLine());
      GenerateObjectMember("alignment", deprecated_type_shape.Alignment());
      GenerateObjectMember("offset", deprecated_field_shape.Offset());
    } else {
      GenerateObjectMember("reserved", true);
      GenerateObjectMember("location", NameSpan(value.span.value()));
    }
  });
}

void JSONGenerator::Generate(const flat::XUnion& value) {
  GenerateObject([&]() {
    GenerateObjectMember("name", value.name, Position::kFirst);
    GenerateObjectMember("location", NameSpan(value.name));
    if (value.attributes)
      GenerateObjectMember("maybe_attributes", value.attributes);
    GenerateObjectMember("members", value.members);
    auto deprecated_type_shape = value.typeshape(WireFormat::kOld);
    GenerateObjectMember("size", deprecated_type_shape.InlineSize());
    GenerateObjectMember("max_out_of_line", deprecated_type_shape.MaxOutOfLine());
    GenerateObjectMember("alignment", deprecated_type_shape.Alignment());
    GenerateObjectMember("max_handles", deprecated_type_shape.MaxHandles());
    GenerateObjectMember("strict", value.strictness == types::Strictness::kStrict);
    GenerateTypeShapes(value);
  });
}

void JSONGenerator::Generate(const flat::XUnion::Member& value) {
  GenerateObject([&]() {
    GenerateObjectMember("ordinal", value.write_ordinal(), Position::kFirst);
    GenerateObjectMember("explicit_ordinal", value.explicit_ordinal);
    if (value.maybe_used) {
      GenerateObjectMember("hashed_ordinal", value.maybe_used->hashed_ordinal);
      assert(!value.span);
      GenerateObjectMember("reserved", false);
      GenerateObjectMember("name", value.maybe_used->name);
      GenerateTypeAndFromTypeAlias(*value.maybe_used->type_ctor);
      GenerateObjectMember("location", NameSpan(value.maybe_used->name));
      if (value.maybe_used->attributes)
        GenerateObjectMember("maybe_attributes", value.maybe_used->attributes);
      auto deprecated_type_shape = value.maybe_used->typeshape(WireFormat::kOld);
      auto deprecated_field_shape = value.maybe_used->fieldshape(WireFormat::kOld);
      GenerateObjectMember("size", deprecated_type_shape.InlineSize());
      GenerateObjectMember("max_out_of_line", deprecated_type_shape.MaxOutOfLine());
      GenerateObjectMember("alignment", deprecated_type_shape.Alignment());
      GenerateObjectMember("offset", deprecated_field_shape.Offset());
    } else {
      GenerateObjectMember("reserved", true);
      GenerateObjectMember("location", NameSpan(value.span.value()));
    }
  });
}

void JSONGenerator::Generate(const flat::TypeConstructor::FromTypeAlias& value) {
  GenerateObject([&]() {
    GenerateObjectMember("name", value.decl->name, Position::kFirst);
    GenerateObjectPunctuation(Position::kSubsequent);
    EmitObjectKey("args");

    // In preparation of template support, it is better to expose a
    // heterogenous argument list to backends, rather than the currently
    // limited internal view.
    EmitArrayBegin();
    if (value.maybe_arg_type) {
      Indent();
      EmitNewlineWithIndent();
      Generate(value.maybe_arg_type->name);
      Outdent();
      EmitNewlineWithIndent();
    }
    EmitArrayEnd();

    GenerateObjectMember("nullable", value.nullability);

    if (value.maybe_size)
      GenerateObjectMember("maybe_size", *value.maybe_size);
  });
}

void JSONGenerator::Generate(const flat::TypeConstructor& value) {
  GenerateObject([&]() {
    GenerateObjectMember("name", value.type ? value.type->name : value.name, Position::kFirst);
    GenerateObjectPunctuation(Position::kSubsequent);
    EmitObjectKey("args");

    // In preparation of template support, it is better to expose a
    // heterogenous argument list to backends, rather than the currently
    // limited internal view.
    EmitArrayBegin();
    if (value.maybe_arg_type_ctor) {
      Indent();
      EmitNewlineWithIndent();
      Generate(*value.maybe_arg_type_ctor);
      Outdent();
      EmitNewlineWithIndent();
    }
    EmitArrayEnd();

    GenerateObjectMember("nullable", value.nullability);

    if (value.maybe_size)
      GenerateObjectMember("maybe_size", value.maybe_size);
    if (value.handle_subtype)
      GenerateObjectMember("maybe_handle_subtype", value.handle_subtype.value());
  });
}

void JSONGenerator::Generate(const flat::TypeAlias& value) {
  GenerateObject([&]() {
    GenerateObjectMember("name", value.name, Position::kFirst);
    GenerateObjectMember("location", NameSpan(value.name));
    if (value.attributes)
      GenerateObjectMember("maybe_attributes", value.attributes);
    GenerateObjectMember("partial_type_ctor", *value.partial_type_ctor);
  });
}

void JSONGenerator::Generate(const flat::Library* library) {
  GenerateObject([&]() {
    auto library_name = flat::LibraryName(library, ".");
    GenerateObjectMember("name", library_name, Position::kFirst);
    GenerateDeclarationsMember(library);
  });
}

void JSONGenerator::GenerateTypeShapes(const flat::Object& object) {
  GenerateTypeShapes("", object);
}

void JSONGenerator::GenerateTypeShapes(std::string prefix, const flat::Object& object) {
  if (prefix.size() > 0) {
    prefix.push_back('_');
  }

  GenerateObjectMember(prefix + "type_shape_old", TypeShape(object, WireFormat::kOld));
  GenerateObjectMember(prefix + "type_shape_v1", TypeShape(object, WireFormat::kV1NoEe));
  GenerateObjectMember(prefix + "type_shape_v1_no_ee", TypeShape(object, WireFormat::kV1NoEe));
}

void JSONGenerator::GenerateFieldShapes(const flat::Struct::Member& struct_member) {
  GenerateObjectMember("field_shape_old", FieldShape(struct_member, WireFormat::kOld));
  GenerateObjectMember("field_shape_v1", FieldShape(struct_member, WireFormat::kV1NoEe));
  GenerateObjectMember("field_shape_v1_no_ee", FieldShape(struct_member, WireFormat::kV1NoEe));
}

void JSONGenerator::GenerateDeclarationsEntry(int count, const flat::Name& name,
                                              std::string_view decl) {
  if (count == 0) {
    Indent();
    EmitNewlineWithIndent();
  } else {
    EmitObjectSeparator();
  }
  EmitObjectKey(NameFlatName(name));
  EmitString(decl);
}

void JSONGenerator::GenerateDeclarationsMember(const flat::Library* library, Position position) {
  GenerateObjectPunctuation(position);
  EmitObjectKey("declarations");
  GenerateObject([&]() {
    int count = 0;
    for (const auto& decl : library->bits_declarations_)
      GenerateDeclarationsEntry(count++, decl->name, "bits");

    for (const auto& decl : library->const_declarations_)
      GenerateDeclarationsEntry(count++, decl->name, "const");

    for (const auto& decl : library->enum_declarations_)
      GenerateDeclarationsEntry(count++, decl->name, "enum");

    for (const auto& decl : library->protocol_declarations_)
      GenerateDeclarationsEntry(count++, decl->name, "interface");

    for (const auto& decl : library->service_declarations_)
      GenerateDeclarationsEntry(count++, decl->name, "service");

    for (const auto& decl : library->struct_declarations_) {
      if (decl->is_request_or_response)
        continue;
      GenerateDeclarationsEntry(count++, decl->name, "struct");
    }

    for (const auto& decl : library->table_declarations_)
      GenerateDeclarationsEntry(count++, decl->name, "table");

    for (const auto& decl : library->union_declarations_)
      GenerateDeclarationsEntry(count++, decl->name, "union");

    for (const auto& decl : library->xunion_declarations_)
      GenerateDeclarationsEntry(count++, decl->name, "xunion");

    for (const auto& decl : library->type_alias_declarations_)
      GenerateDeclarationsEntry(count++, decl->name, "type_alias");
  });
}

namespace {

struct LibraryComparator {
  bool operator()(const flat::Library* lhs, const flat::Library* rhs) const {
    assert(!lhs->name().empty());
    assert(!rhs->name().empty());
    return lhs->name() < rhs->name();
  }
};

std::set<const flat::Library*, LibraryComparator> TransitiveDependencies(
    const flat::Library* library) {
  std::set<const flat::Library*, LibraryComparator> dependencies;
  auto add_dependency = [&](const flat::Library* dep_library) {
    if (!dep_library->HasAttribute("Internal")) {
      dependencies.insert(dep_library);
    }
  };
  for (const auto& dep_library : library->dependencies()) {
    add_dependency(dep_library);
  }
  // Discover additional dependencies that are required to support
  // cross-library protocol composition.
  for (const auto& protocol : library->protocol_declarations_) {
    for (const auto method_with_info : protocol->all_methods) {
      if (auto request = method_with_info.method->maybe_request) {
        for (const auto& member : request->members) {
          if (auto dep_library = member.type_ctor->name.library()) {
            add_dependency(dep_library);
          }
        }
      }
      if (auto response = method_with_info.method->maybe_response) {
        for (const auto& member : response->members) {
          if (auto dep_library = member.type_ctor->name.library()) {
            add_dependency(dep_library);
          }
        }
      }
      add_dependency(method_with_info.method->owning_protocol->name.library());
    }
  }
  dependencies.erase(library);
  return dependencies;
}

}  // namespace

std::ostringstream JSONGenerator::Produce() {
  ResetIndentLevel();
  GenerateObject([&]() {
    GenerateObjectMember("version", std::string_view("0.0.1"), Position::kFirst);

    GenerateObjectMember("name", LibraryName(library_, "."));

    if (auto attributes = library_->attributes(); attributes) {
      GenerateObjectMember("maybe_attributes", *attributes);
    }

    GenerateObjectPunctuation(Position::kSubsequent);
    EmitObjectKey("library_dependencies");
    GenerateArray(TransitiveDependencies(library_));

    GenerateObjectMember("bits_declarations", library_->bits_declarations_);
    GenerateObjectMember("const_declarations", library_->const_declarations_);
    GenerateObjectMember("enum_declarations", library_->enum_declarations_);
    GenerateObjectMember("interface_declarations", library_->protocol_declarations_);
    GenerateObjectMember("service_declarations", library_->service_declarations_);
    GenerateObjectMember("struct_declarations", library_->struct_declarations_);
    GenerateObjectMember("table_declarations", library_->table_declarations_);
    GenerateObjectMember("union_declarations", library_->union_declarations_);
    GenerateObjectMember("xunion_declarations", library_->xunion_declarations_);
    GenerateObjectMember("type_alias_declarations", library_->type_alias_declarations_);

    // The library's declaration_order_ contains all the declarations for all
    // transitive dependencies. The backend only needs the declaration order
    // for this specific library.
    std::vector<std::string> declaration_order;
    for (flat::Decl* decl : library_->declaration_order_) {
      if (decl->kind == flat::Decl::Kind::kStruct) {
        auto struct_decl = static_cast<flat::Struct*>(decl);
        if (struct_decl->is_request_or_response)
          continue;
      }
      if (decl->name.library() == library_)
        declaration_order.push_back(NameFlatName(decl->name));
    }
    GenerateObjectMember("declaration_order", declaration_order);

    GenerateDeclarationsMember(library_);
  });
  GenerateEOF();

  return std::move(json_file_);
}

}  // namespace fidl
