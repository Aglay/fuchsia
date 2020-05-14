// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package rust

import (
	"bytes"
	"encoding/hex"
	"fmt"
	"strconv"
	"strings"

	fidlcommon "fidl/compiler/backend/common"
	fidlir "fidl/compiler/backend/types"
	gidlir "gidl/ir"
	gidlmixer "gidl/mixer"
)

func isPrintableASCII(s string) bool {
	for _, r := range s {
		if r < 0x20 || r > 0x7e {
			return false
		}
	}
	return true
}

func escapeStr(value string) string {
	var (
		buf    bytes.Buffer
		src    = []byte(value)
		dstLen = hex.EncodedLen(len(src))
		dst    = make([]byte, dstLen)
	)
	hex.Encode(dst, src)
	for i := 0; i < dstLen; i += 2 {
		buf.WriteString("\\x")
		buf.WriteByte(dst[i])
		buf.WriteByte(dst[i+1])
	}
	return buf.String()
}

func visit(value interface{}, decl gidlmixer.Declaration) string {
	switch value := value.(type) {
	case bool:
		return strconv.FormatBool(value)
	case int64, uint64, float64:
		switch decl := decl.(type) {
		case gidlmixer.PrimitiveDeclaration:
			suffix := primitiveTypeName(decl.Subtype())
			return fmt.Sprintf("%v%s", value, suffix)
		case *gidlmixer.BitsDecl:
			primitive := visit(value, &decl.Underlying)
			return fmt.Sprintf("%s::from_bits(%v).unwrap()", declName(decl), primitive)
		case *gidlmixer.EnumDecl:
			primitive := visit(value, &decl.Underlying)
			return fmt.Sprintf("%s::from_primitive(%v).unwrap()", declName(decl), primitive)
		}
	case string:
		var expr string
		if isPrintableASCII(value) {
			expr = fmt.Sprintf("String::from(%q)", value)
		} else {
			expr = fmt.Sprintf("std::str::from_utf8(b\"%s\").unwrap().to_string()", escapeStr(value))
		}
		return wrapNullable(decl, expr)
	case gidlir.Record:
		switch decl := decl.(type) {
		case *gidlmixer.StructDecl:
			return onStruct(value, decl)
		case *gidlmixer.TableDecl:
			return onTable(value, decl)
		case *gidlmixer.UnionDecl:
			return onUnion(value, decl)
		}
	case []interface{}:
		switch decl := decl.(type) {
		case *gidlmixer.ArrayDecl:
			return onList(value, decl)
		case *gidlmixer.VectorDecl:
			return onList(value, decl)
		}
	case nil:
		if !decl.IsNullable() {
			panic(fmt.Sprintf("got nil for non-nullable type: %T", decl))
		}
		return "None"
	}
	panic(fmt.Sprintf("not implemented: %T", value))
}

func declName(decl gidlmixer.NamedDeclaration) string {
	return identifierName(decl.Name())
}

// TODO(fxb/39407): Move into a common library outside GIDL.
func identifierName(qualifiedName string) string {
	parts := strings.Split(qualifiedName, "/")
	lastPartsIndex := len(parts) - 1
	for i, part := range parts {
		if i == lastPartsIndex {
			parts[i] = fidlcommon.ToUpperCamelCase(part)
		} else {
			parts[i] = fidlcommon.ToSnakeCase(part)
		}
	}
	return strings.Join(parts, "::")
}

func primitiveTypeName(subtype fidlir.PrimitiveSubtype) string {
	switch subtype {
	case fidlir.Bool:
		return "bool"
	case fidlir.Int8:
		return "i8"
	case fidlir.Uint8:
		return "u8"
	case fidlir.Int16:
		return "i16"
	case fidlir.Uint16:
		return "u16"
	case fidlir.Int32:
		return "i32"
	case fidlir.Uint32:
		return "u32"
	case fidlir.Int64:
		return "i64"
	case fidlir.Uint64:
		return "u64"
	case fidlir.Float32:
		return "f32"
	case fidlir.Float64:
		return "f64"
	default:
		panic(fmt.Sprintf("unexpected subtype %v", subtype))
	}
}

func wrapNullable(decl gidlmixer.Declaration, valueStr string) string {
	if !decl.IsNullable() {
		return valueStr
	}
	switch decl.(type) {
	case *gidlmixer.ArrayDecl, *gidlmixer.VectorDecl, *gidlmixer.StringDecl:
		return fmt.Sprintf("Some(%s)", valueStr)
	case *gidlmixer.StructDecl, *gidlmixer.UnionDecl:
		return fmt.Sprintf("Some(Box::new(%s))", valueStr)
	case *gidlmixer.BoolDecl, *gidlmixer.IntegerDecl, *gidlmixer.FloatDecl, *gidlmixer.TableDecl:
		panic(fmt.Sprintf("decl %v should not be nullable", decl))
	}
	panic(fmt.Sprintf("unexpected decl %v", decl))
}

func onStruct(value gidlir.Record, decl *gidlmixer.StructDecl) string {
	var structFields []string
	providedKeys := make(map[string]struct{}, len(value.Fields))
	for _, field := range value.Fields {
		if field.Key.IsUnknown() {
			panic("unknown field not supported")
		}
		providedKeys[field.Key.Name] = struct{}{}
		fieldName := fidlcommon.ToSnakeCase(field.Key.Name)
		fieldDecl, ok := decl.Field(field.Key.Name)
		if !ok {
			panic(fmt.Sprintf("field %s not found", field.Key.Name))
		}
		fieldValueStr := visit(field.Value, fieldDecl)
		structFields = append(structFields, fmt.Sprintf("%s: %s", fieldName, fieldValueStr))
	}
	for _, key := range decl.FieldNames() {
		if _, ok := providedKeys[key]; !ok {
			fieldName := fidlcommon.ToSnakeCase(key)
			structFields = append(structFields, fmt.Sprintf("%s: None", fieldName))
		}
	}
	valueStr := fmt.Sprintf("%s { %s }", declName(decl), strings.Join(structFields, ", "))
	return wrapNullable(decl, valueStr)
}

func onTable(value gidlir.Record, decl *gidlmixer.TableDecl) string {
	var tableFields []string
	providedKeys := make(map[string]struct{}, len(value.Fields))
	for _, field := range value.Fields {
		if field.Key.IsUnknown() {
			panic("unknown field not supported")
		}
		providedKeys[field.Key.Name] = struct{}{}
		fieldName := fidlcommon.ToSnakeCase(field.Key.Name)
		fieldDecl, ok := decl.Field(field.Key.Name)
		if !ok {
			panic(fmt.Sprintf("field %s not found", field.Key.Name))
		}
		fieldValueStr := visit(field.Value, fieldDecl)
		tableFields = append(tableFields, fmt.Sprintf("%s: Some(%s)", fieldName, fieldValueStr))
	}
	for _, key := range decl.FieldNames() {
		if _, ok := providedKeys[key]; !ok {
			fieldName := fidlcommon.ToSnakeCase(key)
			tableFields = append(tableFields, fmt.Sprintf("%s: None", fieldName))
		}
	}
	valueStr := fmt.Sprintf("%s { %s }", declName(decl), strings.Join(tableFields, ", "))
	return wrapNullable(decl, valueStr)
}

func onUnion(value gidlir.Record, decl *gidlmixer.UnionDecl) string {
	if len(value.Fields) != 1 {
		panic(fmt.Sprintf("union has %d fields, expected 1", len(value.Fields)))
	}
	field := value.Fields[0]
	if field.Key.IsUnknown() {
		panic("unknown field not supported")
	}
	fieldName := fidlcommon.ToUpperCamelCase(field.Key.Name)
	fieldDecl, ok := decl.Field(field.Key.Name)
	if !ok {
		panic(fmt.Sprintf("field %s not found", field.Key.Name))
	}
	fieldValueStr := visit(field.Value, fieldDecl)
	valueStr := fmt.Sprintf("%s::%s(%s)", declName(decl), fieldName, fieldValueStr)
	return wrapNullable(decl, valueStr)
}

func onList(value []interface{}, decl gidlmixer.ListDeclaration) string {
	var elements []string
	elemDecl := decl.Elem()
	for _, item := range value {
		elements = append(elements, visit(item, elemDecl))
	}
	elementsStr := strings.Join(elements, ", ")
	switch decl.(type) {
	case *gidlmixer.ArrayDecl:
		return fmt.Sprintf("[%s]", elementsStr)
	case *gidlmixer.VectorDecl:
		return fmt.Sprintf("vec![%s]", elementsStr)
	}
	panic(fmt.Sprintf("unexpected decl %v", decl))
}
