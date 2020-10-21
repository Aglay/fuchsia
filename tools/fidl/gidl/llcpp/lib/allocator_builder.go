// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package lib

import (
	"fmt"
	"strconv"
	"strings"

	fidlir "go.fuchsia.dev/fuchsia/garnet/go/src/fidl/compiler/backend/types"
	gidlir "go.fuchsia.dev/fuchsia/tools/fidl/gidl/ir"
	gidlmixer "go.fuchsia.dev/fuchsia/tools/fidl/gidl/mixer"
)

// Builds a LLCPP object using std::make_unique.
func BuildValueHeap(value interface{}, decl gidlmixer.Declaration) (string, string) {
	var builder allocatorBuilder
	builder.allocationFunc = "std::make_unique"
	valueVar := builder.visit(value, decl, false)
	valueBuild := builder.String()
	return valueBuild, valueVar
}

// Builds an LLCPP object using fidl::Allocator.
func BuildValueAllocator(allocatorVar string, value interface{}, decl gidlmixer.Declaration) (string, string) {
	var builder allocatorBuilder
	builder.allocationFunc = fmt.Sprintf("%s->make", allocatorVar)
	valueVar := builder.visit(value, decl, false)
	valueBuild := builder.String()
	return valueBuild, valueVar
}

type allocatorBuilder struct {
	allocationFunc string
	strings.Builder
	varidx int
}

func (a *allocatorBuilder) write(format string, vals ...interface{}) {
	a.WriteString(fmt.Sprintf(format, vals...))
}

func (a *allocatorBuilder) newVar() string {
	a.varidx++
	return fmt.Sprintf("var%d", a.varidx)
}

func (a *allocatorBuilder) assignNew(typename string, isPointer bool, str string, vals ...interface{}) string {
	rhs := a.construct(typename, isPointer, str, vals...)
	newVar := a.newVar()
	a.write("auto %s = %s;\n", newVar, rhs)
	return newVar
}

func (a *allocatorBuilder) construct(typename string, isPointer bool, str string, args ...interface{}) string {
	if isPointer {
		return fmt.Sprintf("%s<%s>(%s)", a.allocationFunc, typename, fmt.Sprintf(str, args...))
	}
	return fmt.Sprintf("%s(%s)", typename, fmt.Sprintf(str, args...))
}

func formatPrimitive(value interface{}) string {
	switch value := value.(type) {
	case int64:
		if value == -9223372036854775808 {
			return "-9223372036854775807ll - 1"
		}
		return fmt.Sprintf("%dll", value)
	case uint64:
		return fmt.Sprintf("%dull", value)
	case float64:
		return fmt.Sprintf("%g", value)
	}
	panic("Unreachable")
}

func (a *allocatorBuilder) visit(value interface{}, decl gidlmixer.Declaration, isAlwaysPointer bool) string {
	// Unions, StringView and VectorView in LLCPP represent nullability within the object rather than as
	// as pointer to the object.
	_, isUnion := decl.(*gidlmixer.UnionDecl)
	_, isString := decl.(*gidlmixer.StringDecl)
	_, isVector := decl.(*gidlmixer.VectorDecl)
	isPointer := (decl.IsNullable() && !isUnion && !isString && !isVector) || isAlwaysPointer

	switch value := value.(type) {
	case bool:
		return a.construct(typeName(decl), isPointer, "%t", value)
	case int64, uint64, float64:
		switch decl := decl.(type) {
		case gidlmixer.PrimitiveDeclaration, *gidlmixer.EnumDecl:
			return a.construct(typeName(decl), isPointer, formatPrimitive(value))
		case *gidlmixer.BitsDecl:
			return fmt.Sprintf("static_cast<%s>(%s)", declName(decl), formatPrimitive(value))
		}
	case gidlir.RawFloat:
		switch decl.(*gidlmixer.FloatDecl).Subtype() {
		case fidlir.Float32:
			return fmt.Sprintf("([] { uint32_t u = %#b; float f; memcpy(&f, &u, 4); return f; })()", value)
		case fidlir.Float64:
			return fmt.Sprintf("([] { uint64_t u = %#b; double d; memcpy(&d, &u, 8); return d; })()", value)
		}
	case string:
		if !isPointer {
			// This clause is optional and simplifies the output.
			return strconv.Quote(value)
		}
		return a.construct(typeNameIgnoreNullable(decl), isPointer, "%q", value)
	case gidlir.Handle:
		return fmt.Sprintf("%s(handle_defs[%d])", typeName(decl), value)
	case gidlir.Record:
		switch decl := decl.(type) {
		case *gidlmixer.StructDecl:
			return a.visitStruct(value, decl, isPointer)
		case *gidlmixer.TableDecl:
			return a.visitTable(value, decl, isPointer)
		case *gidlmixer.UnionDecl:
			return a.visitUnion(value, decl, isPointer)
		}
	case []interface{}:
		switch decl := decl.(type) {
		case *gidlmixer.ArrayDecl:
			return a.visitArray(value, decl, isPointer)
		case *gidlmixer.VectorDecl:
			return a.visitVector(value, decl, isPointer)
		}
	case nil:
		return a.construct(typeName(decl), false, "")
	}
	panic(fmt.Sprintf("not implemented: %T", value))
}

func (a *allocatorBuilder) visitStruct(value gidlir.Record, decl *gidlmixer.StructDecl, isPointer bool) string {
	s := a.assignNew(typeNameIgnoreNullable(decl), isPointer, "")
	op := "."
	if isPointer {
		op = "->"
	}

	for _, field := range value.Fields {
		fieldDecl, ok := decl.Field(field.Key.Name)
		if !ok {
			panic(fmt.Sprintf("field %s not found", field.Key.Name))
		}
		fieldRhs := a.visit(field.Value, fieldDecl, false)
		a.write("%s%s%s = %s;\n", s, op, field.Key.Name, fieldRhs)
	}

	return fmt.Sprintf("std::move(%s)", s)
}

func (a *allocatorBuilder) visitTable(value gidlir.Record, decl *gidlmixer.TableDecl, isPointer bool) string {
	frame := a.construct(fmt.Sprintf("%s::Frame", declName(decl)), true, "")
	t := a.assignNew(fmt.Sprintf("%s::Builder", declName(decl)), isPointer, "%s", frame)
	op := "."
	if isPointer {
		op = "->"
	}

	for _, field := range value.Fields {
		fieldDecl, ok := decl.Field(field.Key.Name)
		if !ok {
			panic(fmt.Sprintf("field %s not found", field.Key.Name))
		}
		fieldRhs := a.visit(field.Value, fieldDecl, true)
		a.write("%s%sset_%s(%s);\n", t, op, field.Key.Name, fieldRhs)
	}

	if isPointer {
		return a.construct(typeName(decl), true, "%s->build()", t)
	}
	return fmt.Sprintf("%s.build()", t)
}

func (a *allocatorBuilder) visitUnion(value gidlir.Record, decl *gidlmixer.UnionDecl, isPointer bool) string {
	union := a.assignNew(typeNameIgnoreNullable(decl), isPointer, "")
	op := "."
	if isPointer {
		op = "->"
	}

	if len(value.Fields) == 1 {
		field := value.Fields[0]
		fieldDecl, ok := decl.Field(field.Key.Name)
		if !ok {
			panic(fmt.Sprintf("field %s not found", field.Key.Name))
		}
		fieldRhs := a.visit(field.Value, fieldDecl, true)
		a.write("%s%sset_%s(%s);\n", union, op, field.Key.Name, fieldRhs)
	}

	return fmt.Sprintf("std::move(%s)", union)
}

func (a *allocatorBuilder) visitArray(value []interface{}, decl *gidlmixer.ArrayDecl, isPointer bool) string {
	var elemList []string
	elemDecl := decl.Elem()
	for _, item := range value {
		elemList = append(elemList, a.visit(item, elemDecl, false))
	}
	elems := strings.Join(elemList, ", ")
	if isPointer {
		return fmt.Sprintf("%s<%s>(%s{%s})", a.allocationFunc, typeNameIgnoreNullable(decl), typeNameIgnoreNullable(decl), elems)
	}
	return fmt.Sprintf("%s{%s}", typeName(decl), elems)
}

func (a *allocatorBuilder) visitVector(value []interface{}, decl *gidlmixer.VectorDecl, isPointer bool) string {
	elemDecl := decl.Elem()
	array := a.assignNew(fmt.Sprintf("%s[]", typeName(elemDecl)), true, "%d", len(value))
	for i, item := range value {
		elem := a.visit(item, elemDecl, false)
		a.write("%s[%d] = %s;\n", array, i, elem)
	}
	return a.construct(typeName(decl), isPointer, "std::move(%s), %d", array, len(value))
}
