// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package golang

import (
	"fmt"
	"strconv"
	"strings"

	fidlcommon "fidl/compiler/backend/common"
	fidlir "fidl/compiler/backend/types"
	gidlir "gidl/ir"
	gidlmixer "gidl/mixer"
)

func bytesBuilder(bytes []byte) string {
	var builder strings.Builder
	builder.WriteString("[]byte{\n")
	for i, b := range bytes {
		builder.WriteString(fmt.Sprintf("0x%02x,", b))
		if i%8 == 7 {
			builder.WriteString("\n")
		}
	}
	builder.WriteString("}")
	return builder.String()
}

type goValueBuilder struct {
	strings.Builder
	varidx int

	lastVar string
}

func (b *goValueBuilder) newVar() string {
	b.varidx++
	return fmt.Sprintf("v%d", b.varidx)
}

func (b *goValueBuilder) OnBool(value bool) {
	newVar := b.newVar()
	b.Builder.WriteString(fmt.Sprintf(
		"%s := %t\n", newVar, value))
	b.lastVar = newVar
}

func (b *goValueBuilder) OnInt64(value int64, typ fidlir.PrimitiveSubtype) {
	newVar := b.newVar()
	b.Builder.WriteString(fmt.Sprintf(
		"var %s %s = %d\n", newVar, typ, value))
	b.lastVar = newVar
}

func (b *goValueBuilder) OnUint64(value uint64, typ fidlir.PrimitiveSubtype) {
	newVar := b.newVar()
	b.Builder.WriteString(fmt.Sprintf(
		"var %s %s = %d\n", newVar, typ, value))
	b.lastVar = newVar
}

func (b *goValueBuilder) OnFloat64(value float64, typ fidlir.PrimitiveSubtype) {
	newVar := b.newVar()
	b.Builder.WriteString(fmt.Sprintf(
		"var %s %s = %g\n", newVar, typ, value))
	b.lastVar = newVar
}

func (b *goValueBuilder) OnString(value string, decl *gidlmixer.StringDecl) {
	newVar := b.newVar()
	b.Builder.WriteString(fmt.Sprintf(
		"%s := %s\n", newVar, strconv.Quote(value)))
	if decl.IsNullable() {
		pointee := newVar
		newVar = b.newVar()
		b.Builder.WriteString(fmt.Sprintf("%s := &%s\n", newVar, pointee))
	}
	b.lastVar = newVar
}

func (b *goValueBuilder) OnBits(value interface{}, decl *gidlmixer.BitsDecl) {
	newVar := b.newVar()
	b.Builder.WriteString(fmt.Sprintf(
		"%s := %s(%d)\n", newVar, typeLiteral(decl), value))
	b.lastVar = newVar
}

func (b *goValueBuilder) OnEnum(value interface{}, decl *gidlmixer.EnumDecl) {
	newVar := b.newVar()
	b.Builder.WriteString(fmt.Sprintf(
		"%s := %s(%d)\n", newVar, typeLiteral(decl), value))
	b.lastVar = newVar
}

func (b *goValueBuilder) OnStruct(value gidlir.Record, decl *gidlmixer.StructDecl) {
	b.onRecord(value, decl)
}

func (b *goValueBuilder) onRecord(value gidlir.Record, decl gidlmixer.RecordDeclaration) {
	containerVar := b.newVar()
	b.Builder.WriteString(fmt.Sprintf("%s := %s{}\n", containerVar, typeLiteral(decl)))
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

		switch decl.(type) {
		case *gidlmixer.StructDecl:
			b.Builder.WriteString(fmt.Sprintf(
				"%s.%s = %s\n", containerVar, fidlcommon.ToUpperCamelCase(field.Key.Name), fieldVar))
		default:
			b.Builder.WriteString(fmt.Sprintf(
				"%s.Set%s(%s)\n", containerVar, fidlcommon.ToUpperCamelCase(field.Key.Name), fieldVar))
		}
	}
	b.lastVar = containerVar
}

func (b *goValueBuilder) OnTable(value gidlir.Record, decl *gidlmixer.TableDecl) {
	b.onRecord(value, decl)
}

func (b *goValueBuilder) OnUnion(value gidlir.Record, decl *gidlmixer.UnionDecl) {
	b.onRecord(value, decl)
}

func (b *goValueBuilder) onList(value []interface{}, decl gidlmixer.ListDeclaration) {
	var argStr string
	elemDecl := decl.Elem()
	for _, item := range value {
		gidlmixer.Visit(b, item, elemDecl)
		argStr += b.lastVar + ", "
	}
	sliceVar := b.newVar()
	b.Builder.WriteString(fmt.Sprintf("%s := %s{%s}\n", sliceVar, typeLiteral(decl), argStr))
	b.lastVar = sliceVar
}

func (b *goValueBuilder) OnArray(value []interface{}, decl *gidlmixer.ArrayDecl) {
	b.onList(value, decl)
}

func (b *goValueBuilder) OnVector(value []interface{}, decl *gidlmixer.VectorDecl) {
	b.onList(value, decl)
}

func (b *goValueBuilder) OnNull(decl gidlmixer.Declaration) {
	newVar := b.newVar()
	b.WriteString(fmt.Sprintf("var %s %s = nil\n", newVar, typeName(decl)))
	b.lastVar = newVar
}

func typeName(decl gidlmixer.Declaration) string {
	return typeNameHelper(decl, "*")
}

func typeLiteral(decl gidlmixer.Declaration) string {
	return typeNameHelper(decl, "&")
}

func typeNameHelper(decl gidlmixer.Declaration, pointerPrefix string) string {
	if !decl.IsNullable() {
		pointerPrefix = ""
	}

	switch decl := decl.(type) {
	case gidlmixer.PrimitiveDeclaration:
		return string(decl.Subtype())
	case *gidlmixer.StringDecl:
		return pointerPrefix + "string"
	case *gidlmixer.BitsDecl:
		return identifierName(decl.Name)
	case *gidlmixer.EnumDecl:
		return identifierName(decl.Name)
	case *gidlmixer.StructDecl:
		return pointerPrefix + identifierName(decl.Name)
	case *gidlmixer.TableDecl:
		return pointerPrefix + identifierName(decl.Name)
	case *gidlmixer.UnionDecl:
		return pointerPrefix + identifierName(decl.Name)
	case *gidlmixer.ArrayDecl:
		return fmt.Sprintf("[%d]%s", decl.Size(), typeName(decl.Elem()))
	case *gidlmixer.VectorDecl:
		return fmt.Sprintf("%s[]%s", pointerPrefix, typeName(decl.Elem()))
	default:
		panic("unhandled case")
	}
}

// TODO(fxb/39407): Such utilities (and their accompanying tests) would be
// useful as part of fidlcommon or fidlir to do FIDL-to-<target_lang>
// conversion.
func identifierName(eci fidlir.EncodedCompoundIdentifier) string {
	parts := strings.Split(string(eci), "/")
	lastPartsIndex := len(parts) - 1
	for i, part := range parts {
		if i == lastPartsIndex {
			parts[i] = fidlcommon.ToUpperCamelCase(part)
		} else {
			parts[i] = fidlcommon.ToSnakeCase(part)
		}
	}
	return strings.Join(parts, ".")
}

// Go errors are defined in third_party/go/src/syscall/zx/fidl/errors.go
var goErrorCodeNames = map[gidlir.ErrorCode]string{
	gidlir.StringTooLong:              "ErrStringTooLong",
	gidlir.NonEmptyStringWithNullBody: "ErrUnexpectedNullRef",
	gidlir.StrictUnionFieldNotSet:     "ErrInvalidXUnionTag",
	gidlir.StrictUnionUnknownField:    "ErrInvalidXUnionTag",
}

func goErrorCode(code gidlir.ErrorCode) (string, error) {
	if str, ok := goErrorCodeNames[code]; ok {
		return fmt.Sprintf("fidl.%s", str), nil
	}
	return "", fmt.Errorf("no go error string defined for error code %s", code)
}
