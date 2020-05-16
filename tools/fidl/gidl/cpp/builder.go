// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package cpp

import (
	"fmt"
	"strconv"
	"strings"

	fidlir "fidl/compiler/backend/types"
	gidlir "gidl/ir"
	gidlmixer "gidl/mixer"
)

func newCppValueBuilder() cppValueBuilder {
	return cppValueBuilder{}
}

type cppValueBuilder struct {
	strings.Builder

	varidx int
}

func (b *cppValueBuilder) newVar() string {
	b.varidx++
	return fmt.Sprintf("v%d", b.varidx)
}

func (b *cppValueBuilder) visit(value interface{}, decl gidlmixer.Declaration) string {
	switch value := value.(type) {
	case bool:
		return fmt.Sprintf("%t", value)
	case int64:
		intString := fmt.Sprintf("%dll", value)
		if value == -9223372036854775808 {
			intString = "-9223372036854775807ll - 1"
		}
		switch decl := decl.(type) {
		case *gidlmixer.IntegerDecl:
			return intString
		case *gidlmixer.BitsDecl:
			return fmt.Sprintf("%s(%s)", typeName(decl), intString)
		case *gidlmixer.EnumDecl:
			return fmt.Sprintf("%s(%s)", typeName(decl), intString)
		default:
			panic(fmt.Sprintf("int64 value has non-integer decl: %T", decl))
		}
	case uint64:
		switch decl := decl.(type) {
		case *gidlmixer.IntegerDecl:
			return fmt.Sprintf("%dull", value)
		case *gidlmixer.BitsDecl:
			return fmt.Sprintf("%s(%dull)", typeName(decl), value)
		case *gidlmixer.EnumDecl:
			return fmt.Sprintf("%s(%dull)", typeName(decl), value)
		default:
			panic(fmt.Sprintf("uint64 value has non-integer decl: %T", decl))
		}
	case float64:
		return fmt.Sprintf("%g", value)
	case string:
		// TODO(fxb/39686) Consider Go/C++ escape sequence differences
		return strconv.Quote(value)
	case gidlir.Record:
		return b.visitRecord(value, decl.(gidlmixer.RecordDeclaration))
	case []interface{}:
		switch decl := decl.(type) {
		case *gidlmixer.ArrayDecl:
			return b.visitArray(value, decl)
		case *gidlmixer.VectorDecl:
			return b.visitVector(value, decl)
		default:
			panic("unknown list decl type")
		}
	case nil:
		return fmt.Sprintf("%s()", typeName(decl))
	default:
		panic(fmt.Sprintf("%T not implemented", value))
	}
}

func (b *cppValueBuilder) visitRecord(value gidlir.Record, decl gidlmixer.RecordDeclaration) string {
	containerVar := b.newVar()
	nullable := decl.IsNullable()
	if nullable {
		b.Builder.WriteString(fmt.Sprintf(
			"%s %s = std::make_unique<%s>();\n", typeName(decl), containerVar, declName(decl)))
	} else {
		b.Builder.WriteString(fmt.Sprintf("%s %s;\n", typeName(decl), containerVar))
	}

	for _, field := range value.Fields {
		if field.Key.IsUnknown() {
			panic("unknown field not supported")
		}
		b.Builder.WriteString("\n")

		fieldDecl, ok := decl.Field(field.Key.Name)
		if !ok {
			panic(fmt.Sprintf("field %s not found", field.Key.Name))
		}
		fieldVar := b.visit(field.Value, fieldDecl)

		accessor := "."
		if nullable {
			accessor = "->"
		}

		switch decl.(type) {
		case *gidlmixer.StructDecl:
			b.Builder.WriteString(fmt.Sprintf(
				"%s%s%s = %s;\n", containerVar, accessor, field.Key.Name, fieldVar))
		default:
			b.Builder.WriteString(fmt.Sprintf(
				"%s%sset_%s(%s);\n", containerVar, accessor, field.Key.Name, fieldVar))
		}
	}
	return fmt.Sprintf("std::move(%s)", containerVar)
}

func (b *cppValueBuilder) visitArray(value []interface{}, decl *gidlmixer.ArrayDecl) string {
	var elements []string
	elemDecl := decl.Elem()
	for _, item := range value {
		elements = append(elements, fmt.Sprintf("%s", b.visit(item, elemDecl)))
	}
	// Populate the array using aggregate initialization.
	return fmt.Sprintf("%s{%s}",
		typeName(decl), strings.Join(elements, ", "))
}

func (b *cppValueBuilder) visitVector(value []interface{}, decl *gidlmixer.VectorDecl) string {
	var elements []string
	elemDecl := decl.Elem()
	for _, item := range value {
		elements = append(elements, b.visit(item, elemDecl))
	}
	vectorVar := b.newVar()
	// Populate the vector using push_back. We can't use an initializer list
	// because they always copy, which breaks if the element is a unique_ptr.
	b.Builder.WriteString(fmt.Sprintf("%s %s;\n", typeName(decl), vectorVar))
	for _, element := range elements {
		b.Builder.WriteString(fmt.Sprintf("%s.push_back(%s);\n", vectorVar, element))
	}
	return fmt.Sprintf("std::move(%s)", vectorVar)
}

func typeName(decl gidlmixer.Declaration) string {
	switch decl := decl.(type) {
	case gidlmixer.PrimitiveDeclaration:
		return primitiveTypeName(decl.Subtype())
	case gidlmixer.NamedDeclaration:
		if decl.IsNullable() {
			return fmt.Sprintf("std::unique_ptr<%s>", declName(decl))
		}
		return declName(decl)
	case *gidlmixer.StringDecl:
		if decl.IsNullable() {
			return "::fidl::StringPtr"
		}
		return "std::string"
	case *gidlmixer.ArrayDecl:
		return fmt.Sprintf("std::array<%s, %d>", typeName(decl.Elem()), decl.Size())
	case *gidlmixer.VectorDecl:
		if decl.IsNullable() {
			return fmt.Sprintf("::fidl::VectorPtr<%s>", typeName(decl.Elem()))
		}
		return fmt.Sprintf("std::vector<%s>", typeName(decl.Elem()))
	default:
		panic("unhandled case")
	}
}

func declName(decl gidlmixer.NamedDeclaration) string {
	parts := strings.Split(decl.Name(), "/")
	return strings.Join(parts, "::")
}

func primitiveTypeName(subtype fidlir.PrimitiveSubtype) string {
	switch subtype {
	case fidlir.Bool:
		return "bool"
	case fidlir.Uint8, fidlir.Uint16, fidlir.Uint32, fidlir.Uint64,
		fidlir.Int8, fidlir.Int16, fidlir.Int32, fidlir.Int64:
		return fmt.Sprintf("%s_t", subtype)
	case fidlir.Float32:
		return "float"
	case fidlir.Float64:
		return "double"
	default:
		panic(fmt.Sprintf("unexpected subtype %s", subtype))
	}
}
