// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package llcpp

import (
	"fmt"
	"io"
	"strconv"
	"strings"
	"text/template"

	fidlcommon "fidl/compiler/backend/common"
	fidlir "fidl/compiler/backend/types"
	gidlir "gidl/ir"
	gidlmixer "gidl/mixer"
)

var tmpl = template.Must(template.New("tmpl").Parse(`
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include <conformance/llcpp/fidl.h>
#include <gtest/gtest.h>

#include "src/lib/fidl/llcpp/tests/test_utils.h"

{{ range .EncodeSuccessCases }}
TEST(Conformance, {{ .Name }}_Encode) {
	{{ .ValueBuild }}
	const auto expected = {{ .Bytes }};
	EXPECT_TRUE(llcpp_conformance_utils::EncodeSuccess(&{{ .ValueVar }}, expected));
}
{{ end }}

{{ range .DecodeSuccessCases }}
TEST(Conformance, {{ .Name }}_Decode) {
	{{ .ValueBuild }}
	auto bytes = {{ .Bytes }};
	EXPECT_TRUE(llcpp_conformance_utils::DecodeSuccess(&{{ .ValueVar }}, std::move(bytes)));
}
{{ end }}

{{ range .EncodeFailureCases }}
TEST(Conformance, {{ .Name }}_Encode_Failure) {
	{{ .ValueBuild }}
	EXPECT_TRUE(llcpp_conformance_utils::EncodeFailure(&{{ .ValueVar }}, {{ .ErrorCode }}));
}
{{ end }}

{{ range .DecodeFailureCases }}
TEST(Conformance, {{ .Name }}_Decode_Failure) {
	auto bytes = {{ .Bytes }};
	EXPECT_TRUE(llcpp_conformance_utils::DecodeFailure<{{ .ValueType }}>(std::move(bytes), {{ .ErrorCode }}));
}
{{ end }}
`))

type tmplInput struct {
	EncodeSuccessCases []encodeSuccessCase
	DecodeSuccessCases []decodeSuccessCase
	EncodeFailureCases []encodeFailureCase
	DecodeFailureCases []decodeFailureCase
}

type encodeSuccessCase struct {
	Name, ValueBuild, ValueVar, Bytes string
}

type decodeSuccessCase struct {
	Name, ValueBuild, ValueVar, Bytes string
}

type encodeFailureCase struct {
	Name, ValueBuild, ValueVar, ErrorCode string
}

type decodeFailureCase struct {
	Name, ValueType, Bytes, ErrorCode string
}

// Generate generates Low-Level C++ tests.
func Generate(wr io.Writer, gidl gidlir.All, fidl fidlir.Root) error {
	schema := gidlmixer.BuildSchema(fidl)
	encodeSuccessCases, err := encodeSuccessCases(gidl.EncodeSuccess, schema)
	if err != nil {
		return err
	}
	decodeSuccessCases, err := decodeSuccessCases(gidl.DecodeSuccess, schema)
	if err != nil {
		return err
	}
	encodeFailureCases, err := encodeFailureCases(gidl.EncodeFailure, schema)
	if err != nil {
		return err
	}
	decodeFailureCases, err := decodeFailureCases(gidl.DecodeFailure, schema)
	if err != nil {
		return err
	}
	return tmpl.Execute(wr, tmplInput{
		EncodeSuccessCases: encodeSuccessCases,
		DecodeSuccessCases: decodeSuccessCases,
		EncodeFailureCases: encodeFailureCases,
		DecodeFailureCases: decodeFailureCases,
	})
}

func encodeSuccessCases(gidlEncodeSuccesses []gidlir.EncodeSuccess, schema gidlmixer.Schema) ([]encodeSuccessCase, error) {
	var encodeSuccessCases []encodeSuccessCase
	for _, encodeSuccess := range gidlEncodeSuccesses {
		decl, err := schema.ExtractDeclaration(encodeSuccess.Value)
		if err != nil {
			return nil, fmt.Errorf("encode success %s: %s", encodeSuccess.Name, err)
		}
		if gidlir.ContainsUnknownField(encodeSuccess.Value) {
			continue
		}
		valueBuild, valueVar := buildValue(encodeSuccess.Value, decl)
		for _, encoding := range encodeSuccess.Encodings {
			if !wireFormatSupported(encoding.WireFormat) {
				continue
			}
			encodeSuccessCases = append(encodeSuccessCases, encodeSuccessCase{
				Name:       testCaseName(encodeSuccess.Name, encoding.WireFormat),
				ValueBuild: valueBuild,
				ValueVar:   valueVar,
				Bytes:      bytesBuilder(encoding.Bytes),
			})
		}
	}
	return encodeSuccessCases, nil
}

func decodeSuccessCases(gidlDecodeSuccesses []gidlir.DecodeSuccess, schema gidlmixer.Schema) ([]decodeSuccessCase, error) {
	var decodeSuccessCases []decodeSuccessCase
	for _, decodeSuccess := range gidlDecodeSuccesses {
		decl, err := schema.ExtractDeclaration(decodeSuccess.Value)
		if err != nil {
			return nil, fmt.Errorf("decode success %s: %s", decodeSuccess.Name, err)
		}
		if gidlir.ContainsUnknownField(decodeSuccess.Value) {
			continue
		}
		valueBuild, valueVar := buildValue(decodeSuccess.Value, decl)
		for _, encoding := range decodeSuccess.Encodings {
			if !wireFormatSupported(encoding.WireFormat) {
				continue
			}
			decodeSuccessCases = append(decodeSuccessCases, decodeSuccessCase{
				Name:       testCaseName(decodeSuccess.Name, encoding.WireFormat),
				ValueBuild: valueBuild,
				ValueVar:   valueVar,
				Bytes:      bytesBuilder(encoding.Bytes),
			})
		}
	}
	return decodeSuccessCases, nil
}

func encodeFailureCases(gidlEncodeFailurees []gidlir.EncodeFailure, schema gidlmixer.Schema) ([]encodeFailureCase, error) {
	var encodeFailureCases []encodeFailureCase
	for _, encodeFailure := range gidlEncodeFailurees {
		decl, err := schema.ExtractDeclarationUnsafe(encodeFailure.Value)
		if err != nil {
			return nil, fmt.Errorf("encode failure %s: %s", encodeFailure.Name, err)
		}
		valueBuild, valueVar := buildValue(encodeFailure.Value, decl)
		errorCode := llcppErrorCode(encodeFailure.Err)
		for _, wireFormat := range encodeFailure.WireFormats {
			if !wireFormatSupported(wireFormat) {
				continue
			}
			encodeFailureCases = append(encodeFailureCases, encodeFailureCase{
				Name:       encodeFailure.Name,
				ValueBuild: valueBuild,
				ValueVar:   valueVar,
				ErrorCode:  errorCode,
			})
		}
	}
	return encodeFailureCases, nil
}

func decodeFailureCases(gidlDecodeFailurees []gidlir.DecodeFailure, schema gidlmixer.Schema) ([]decodeFailureCase, error) {
	var decodeFailureCases []decodeFailureCase
	for _, decodeFailure := range gidlDecodeFailurees {
		_, err := schema.ExtractDeclarationByName(decodeFailure.Type)
		if err != nil {
			return nil, fmt.Errorf("decode failure %s: %s", decodeFailure.Name, err)
		}
		valueType := llcppType(decodeFailure.Type)
		errorCode := llcppErrorCode(decodeFailure.Err)
		for _, encoding := range decodeFailure.Encodings {
			if !wireFormatSupported(encoding.WireFormat) {
				continue
			}
			decodeFailureCases = append(decodeFailureCases, decodeFailureCase{
				Name:      decodeFailure.Name,
				ValueType: valueType,
				Bytes:     bytesBuilder(encoding.Bytes),
				ErrorCode: errorCode,
			})
		}
	}
	return decodeFailureCases, nil
}

func wireFormatSupported(wireFormat gidlir.WireFormat) bool {
	return wireFormat == gidlir.V1WireFormat
}

func testCaseName(baseName string, wireFormat gidlir.WireFormat) string {
	return fmt.Sprintf("%s_%s", baseName, fidlcommon.ToUpperCamelCase(wireFormat.String()))
}

func bytesBuilder(bytes []byte) string {
	var builder strings.Builder
	builder.WriteString("std::vector<uint8_t>{\n")
	for i, b := range bytes {
		builder.WriteString(fmt.Sprintf("0x%02x,", b))
		if i%8 == 7 {
			builder.WriteString("\n")
		}
	}
	builder.WriteString("}")
	return builder.String()
}

func buildValue(value interface{}, decl gidlmixer.Declaration) (string, string) {
	var builder llcppValueBuilder
	gidlmixer.Visit(&builder, value, decl)
	valueBuild := builder.String()
	valueVar := builder.lastVar
	return valueBuild, valueVar
}

type llcppValueBuilder struct {
	strings.Builder
	varidx  int
	lastVar string
}

func (b *llcppValueBuilder) newVar() string {
	b.varidx++
	return fmt.Sprintf("v%d", b.varidx)
}

func (b *llcppValueBuilder) OnBool(value bool) {
	newVar := b.newVar()
	// FIDL_ALIGNDECL is needed to avoid tracking_ptrs that don't have an LSB of 0.
	b.Builder.WriteString(fmt.Sprintf("FIDL_ALIGNDECL bool %s = %t;\n", newVar, value))
	b.lastVar = newVar
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

func (b *llcppValueBuilder) OnInt64(value int64, typ fidlir.PrimitiveSubtype) {
	newVar := b.newVar()
	if value == -9223372036854775808 {
		// There are no negative integer literals in C++, so need to use arithmetic to create the minimum value.
		// FIDL_ALIGNDECL is needed to avoid tracking_ptrs that don't have an LSB of 0.
		b.Builder.WriteString(fmt.Sprintf("FIDL_ALIGNDECL %s %s = -9223372036854775807ll - 1;\n", primitiveTypeName(typ), newVar))
	} else {
		// FIDL_ALIGNDECL is needed to avoid tracking_ptrs that don't have an LSB of 0.
		b.Builder.WriteString(fmt.Sprintf("FIDL_ALIGNDECL %s %s = %dll;\n", primitiveTypeName(typ), newVar, value))
	}
	b.lastVar = newVar
}

func (b *llcppValueBuilder) OnUint64(value uint64, subtype fidlir.PrimitiveSubtype) {
	newVar := b.newVar()
	// FIDL_ALIGNDECL is needed to avoid tracking_ptrs that don't have an LSB of 0.
	b.Builder.WriteString(fmt.Sprintf("FIDL_ALIGNDECL %s %s = %dull;\n", primitiveTypeName(subtype), newVar, value))
	b.lastVar = newVar
}

func (b *llcppValueBuilder) OnFloat64(value float64, subtype fidlir.PrimitiveSubtype) {
	newVar := b.newVar()
	// FIDL_ALIGNDECL is needed to avoid tracking_ptrs that don't have an LSB of 0.
	b.Builder.WriteString(fmt.Sprintf("FIDL_ALIGNDECL %s %s = %g;\n", primitiveTypeName(subtype), newVar, value))
	b.lastVar = newVar
}

func (b *llcppValueBuilder) OnString(value string, decl *gidlmixer.StringDecl) {
	newVar := b.newVar()
	// TODO(fxb/39686) Consider Go/C++ escape sequence differences
	b.Builder.WriteString(fmt.Sprintf(
		"fidl::StringView %s(%s, %d)\n;", newVar, strconv.Quote(value), len(value)))
	b.lastVar = newVar
}

func (b *llcppValueBuilder) OnStruct(value gidlir.Record, decl *gidlmixer.StructDecl) {
	containerVar := b.newVar()
	b.Builder.WriteString(fmt.Sprintf(
		"llcpp::conformance::%s %s{};\n", value.Name, containerVar))
	for _, field := range value.Fields {
		fieldDecl, ok := decl.Field(field.Key.Name)
		if !ok {
			panic(fmt.Sprintf("field %s not found", field.Key.Name))
		}
		gidlmixer.Visit(b, field.Value, fieldDecl)
		b.Builder.WriteString(fmt.Sprintf(
			"%s.%s = std::move(%s);\n", containerVar, field.Key.Name, b.lastVar))
	}
	if decl.IsNullable() {
		alignedVar := b.newVar()
		b.Builder.WriteString(fmt.Sprintf("fidl::aligned<%s> %s = std::move(%s);\n", typeNameIgnoreNullable(decl), alignedVar, containerVar))
		unownedVar := b.newVar()
		b.Builder.WriteString(fmt.Sprintf("%s %s = fidl::unowned_ptr(&%s);\n", typeName(decl), unownedVar, alignedVar))
		b.lastVar = unownedVar
	} else {
		b.lastVar = containerVar
	}
}

func (b *llcppValueBuilder) OnTable(value gidlir.Record, decl *gidlmixer.TableDecl) {
	frameVar := b.newVar()

	b.Builder.WriteString(fmt.Sprintf(
		"auto %s = llcpp::conformance::%s::Frame();\n", frameVar, value.Name))

	builderVar := b.newVar()

	b.Builder.WriteString(fmt.Sprintf(
		"auto %s = llcpp::conformance::%s::Builder(fidl::unowned_ptr(&%s));\n", builderVar, value.Name, frameVar))

	for _, field := range value.Fields {
		if field.Key.IsUnknown() {
			panic("unknown field not supported")
		}
		fieldDecl, ok := decl.Field(field.Key.Name)
		if !ok {
			panic(fmt.Sprintf("field %s not found", field.Key.Name))
		}
		gidlmixer.Visit(b, field.Value, fieldDecl)
		fieldVar := b.lastVar
		alignedVar := b.newVar()
		b.Builder.WriteString(fmt.Sprintf("fidl::aligned<%s> %s = std::move(%s);\n", typeName(fieldDecl), alignedVar, fieldVar))
		b.Builder.WriteString(fmt.Sprintf(
			"%s.set_%s(fidl::unowned_ptr(&%s));\n", builderVar, field.Key.Name, alignedVar))

	}
	tableVar := b.newVar()
	b.Builder.WriteString(fmt.Sprintf(
		"auto %s = %s.build();\n", tableVar, builderVar))

	b.lastVar = tableVar
}

func (b *llcppValueBuilder) OnUnion(value gidlir.Record, decl *gidlmixer.UnionDecl) {
	containerVar := b.newVar()

	b.Builder.WriteString(fmt.Sprintf(
		"llcpp::conformance::%s %s;\n", value.Name, containerVar))

	for _, field := range value.Fields {
		if field.Key.IsUnknown() {
			panic("unknown field not supported")
		}
		fieldDecl, ok := decl.Field(field.Key.Name)
		if !ok {
			panic(fmt.Sprintf("field %s not found", field.Key.Name))
		}
		gidlmixer.Visit(b, field.Value, fieldDecl)
		fieldVar := b.lastVar
		alignedVar := b.newVar()
		b.Builder.WriteString(fmt.Sprintf("fidl::aligned<%s> %s = std::move(%s);\n", typeName(fieldDecl), alignedVar, fieldVar))
		b.Builder.WriteString(fmt.Sprintf(
			"%s.set_%s(fidl::unowned_ptr(&%s));\n", containerVar, field.Key.Name, alignedVar))
	}
	b.lastVar = containerVar
}

func (b *llcppValueBuilder) OnArray(value []interface{}, decl *gidlmixer.ArrayDecl) {
	var elements []string
	elemDecl := decl.Elem()
	for _, item := range value {
		gidlmixer.Visit(b, item, elemDecl)
		elements = append(elements, fmt.Sprintf("std::move(%s)", b.lastVar))
	}
	sliceVar := b.newVar()
	b.Builder.WriteString(fmt.Sprintf("FIDL_ALIGNDECL auto %s = %s{%s};\n",
		sliceVar, typeName(decl), strings.Join(elements, ", ")))
	b.lastVar = sliceVar
}

func (b *llcppValueBuilder) OnVector(value []interface{}, decl *gidlmixer.VectorDecl) {
	if len(value) == 0 {
		sliceVar := b.newVar()
		b.Builder.WriteString(fmt.Sprintf("auto %s = %s();\n",
			sliceVar, typeName(decl)))
		b.lastVar = sliceVar
		return

	}
	var elements []string
	elemDecl := decl.Elem()
	for _, item := range value {
		gidlmixer.Visit(b, item, elemDecl)
		elements = append(elements, fmt.Sprintf("std::move(%s)", b.lastVar))
	}
	arrayVar := b.newVar()
	b.Builder.WriteString(fmt.Sprintf("auto %s = fidl::Array<%s, %d>{%s};\n",
		arrayVar, typeName(elemDecl), len(elements), strings.Join(elements, ", ")))
	sliceVar := b.newVar()
	b.Builder.WriteString(fmt.Sprintf("auto %s = %s(fidl::unowned_ptr(%s.data()), %d);\n",
		sliceVar, typeName(decl), arrayVar, len(elements)))
	b.lastVar = sliceVar
}

func (b *llcppValueBuilder) OnNull(decl gidlmixer.Declaration) {
	newVar := b.newVar()
	b.Builder.WriteString(fmt.Sprintf("%s %s{};\n", typeName(decl), newVar))
	b.lastVar = newVar
}

func typeNameImpl(decl gidlmixer.Declaration, ignoreNullable bool) string {
	switch decl := decl.(type) {
	case gidlmixer.PrimitiveDeclaration:
		return primitiveTypeName(decl.Subtype())
	case *gidlmixer.StringDecl:
		return "fidl::StringView"
	case *gidlmixer.StructDecl:
		if !ignoreNullable && decl.IsNullable() {
			return fmt.Sprintf("fidl::tracking_ptr<%s>", identifierName(decl.Name))
		}
		return identifierName(decl.Name)
	case *gidlmixer.TableDecl:
		return identifierName(decl.Name)
	case *gidlmixer.UnionDecl:
		return identifierName(decl.Name)
	case *gidlmixer.ArrayDecl:
		return fmt.Sprintf("fidl::Array<%s, %d>", typeName(decl.Elem()), decl.Size())
	case *gidlmixer.VectorDecl:
		return fmt.Sprintf("fidl::VectorView<%s>", typeName(decl.Elem()))
	default:
		panic("unhandled case")
	}
}

func typeName(decl gidlmixer.Declaration) string {
	return typeNameImpl(decl, false)
}

func typeNameIgnoreNullable(decl gidlmixer.Declaration) string {
	return typeNameImpl(decl, true)
}

func identifierName(eci fidlir.EncodedCompoundIdentifier) string {
	parts := strings.Split(string(eci), "/")
	if parts[0] == "conformance" {
		parts = append([]string{"llcpp"}, parts...)
	}
	return strings.Join(parts, "::")
}

func llcppType(gidlTypeString string) string {
	return "llcpp::conformance::" + gidlTypeString
}

func llcppErrorCode(code gidlir.ErrorCode) string {
	// TODO(fxb/35381) Implement different codes for different FIDL error cases.
	return "ZX_ERR_INVALID_ARGS"
}
