// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package mixer

import (
	"fmt"
	"math"

	fidlir "fidl/compiler/backend/types"
	gidlir "gidl/ir"
)

// ValueVisitor is an API that walks GIDL values.
type ValueVisitor interface {
	OnBool(value bool)
	OnInt64(value int64, typ fidlir.PrimitiveSubtype)
	OnUint64(value uint64, typ fidlir.PrimitiveSubtype)
	OnFloat64(value float64, typ fidlir.PrimitiveSubtype)
	OnString(value string, decl *StringDecl)
	OnStruct(value gidlir.Record, decl *StructDecl)
	OnTable(value gidlir.Record, decl *TableDecl)
	OnUnion(value gidlir.Record, decl *UnionDecl)
	OnArray(value []interface{}, decl *ArrayDecl)
	OnVector(value []interface{}, decl *VectorDecl)
	OnNull(decl Declaration)
}

// Visit is the entry point into visiting a value, it dispatches appropriately
// into the visitor.
func Visit(visitor ValueVisitor, value interface{}, decl Declaration) {
	switch value := value.(type) {
	case bool:
		visitor.OnBool(value)
	case int64:
		visitor.OnInt64(value, decl.(*IntegerDecl).Subtype())
	case uint64:
		visitor.OnUint64(value, decl.(*IntegerDecl).Subtype())
	case float64:
		visitor.OnFloat64(value, decl.(*FloatDecl).Subtype())
	case string:
		switch decl := decl.(type) {
		case *StringDecl:
			visitor.OnString(value, decl)
		default:
			panic(fmt.Sprintf("string value has non-string decl: %T", decl))
		}
	case gidlir.Record:
		switch decl := decl.(type) {
		case *StructDecl:
			visitor.OnStruct(value, decl)
		case *TableDecl:
			visitor.OnTable(value, decl)
		case *UnionDecl:
			visitor.OnUnion(value, decl)
		default:
			panic(fmt.Sprintf("expected %T, got %T: %v", decl, value, value))
		}
	case []interface{}:
		switch decl := decl.(type) {
		case *ArrayDecl:
			visitor.OnArray(value, decl)
		case *VectorDecl:
			visitor.OnVector(value, decl)
		default:
			panic(fmt.Sprintf("not implemented: %T", decl))
		}
	case nil:
		if !decl.IsNullable() {
			panic(fmt.Sprintf("got nil for non-nullable type: %T", decl))
		}
		visitor.OnNull(decl)
	default:
		panic(fmt.Sprintf("not implemented: %T", value))
	}
}

// Declaration is the GIDL-level concept of a FIDL type. It is more convenient
// to work with in GIDL backends than fidlir.Type. It also provides logic for
// testing if a GIDL value conforms to the declaration.
type Declaration interface {
	// IsNullable returns true for nullable types. For example, it returns false
	// for string and true for string?.
	IsNullable() bool

	// conforms verifies that the value conforms to this declaration.
	conforms(value interface{}) error
}

// Assert that wrappers conform to the Declaration interface.
var _ = []Declaration{
	&BoolDecl{},
	&IntegerDecl{},
	&FloatDecl{},
	&StringDecl{},
	&StructDecl{},
	&TableDecl{},
	&UnionDecl{},
	&ArrayDecl{},
	&VectorDecl{},
}

type PrimitiveDeclaration interface {
	Declaration
	// Subtype returns the primitive subtype (bool, uint32, float64, etc.).
	Subtype() fidlir.PrimitiveSubtype
}

// Assert that wrappers conform to the PrimitiveDeclaration interface.
var _ = []PrimitiveDeclaration{
	&BoolDecl{},
	&IntegerDecl{},
	&FloatDecl{},
}

type RecordDeclaration interface {
	Declaration
	// Field returns the declaration for the field with the given name. It
	// returns false if no field with that name exists.
	Field(name string) (Declaration, bool)
}

// Assert that wrappers conform to the RecordDeclaration interface.
var _ = []RecordDeclaration{
	&StructDecl{},
	&TableDecl{},
	&UnionDecl{},
}

type ListDeclaration interface {
	Declaration
	// Elem returns the declaration for the list's element type.
	Elem() Declaration
}

// Assert that wrappers conform to the ListDeclaration interface.
var _ = []ListDeclaration{
	&ArrayDecl{},
	&VectorDecl{},
}

// Helper struct for implementing IsNullable on types that are never nullable.
type NeverNullable struct{}

func (NeverNullable) IsNullable() bool {
	return false
}

type BoolDecl struct {
	NeverNullable
}

func (decl *BoolDecl) Subtype() fidlir.PrimitiveSubtype {
	return fidlir.Bool
}

func (decl *BoolDecl) conforms(value interface{}) error {
	switch value.(type) {
	default:
		return fmt.Errorf("expecting bool, found %T (%s)", value, value)
	case bool:
		return nil
	}
}

type IntegerDecl struct {
	NeverNullable
	subtype fidlir.PrimitiveSubtype
	lower   int64
	upper   uint64
}

func (decl *IntegerDecl) Subtype() fidlir.PrimitiveSubtype {
	return decl.subtype
}

func (decl *IntegerDecl) conforms(value interface{}) error {
	switch value := value.(type) {
	default:
		return fmt.Errorf("expecting int64 or uint64, found %T (%s)", value, value)
	case int64:
		if value < 0 {
			if value < decl.lower {
				return fmt.Errorf("out-of-bounds %d", value)
			}
		} else {
			if decl.upper < uint64(value) {
				return fmt.Errorf("out-of-bounds %d", value)
			}
		}
		return nil
	case uint64:
		if decl.upper < value {
			return fmt.Errorf("out-of-bounds %d", value)
		}
		return nil
	}
}

type FloatDecl struct {
	NeverNullable
	subtype fidlir.PrimitiveSubtype
}

func (decl *FloatDecl) Subtype() fidlir.PrimitiveSubtype {
	return decl.subtype
}

func (decl *FloatDecl) conforms(value interface{}) error {
	switch value := value.(type) {
	default:
		return fmt.Errorf("expecting float64, found %T (%s)", value, value)
	case float64:
		// TODO(fxb/43020): Allow these once each backend supports them.
		if math.IsNaN(value) {
			return fmt.Errorf("NaN not supported: %v", value)
		}
		if math.IsInf(value, 0) {
			return fmt.Errorf("infinity not supported: %v", value)
		}
		return nil
	}
}

type StringDecl struct {
	bound    *int
	nullable bool
}

func (decl *StringDecl) IsNullable() bool {
	return decl.nullable
}

func (decl *StringDecl) conforms(value interface{}) error {
	switch value := value.(type) {
	default:
		return fmt.Errorf("expecting string, found %T (%s)", value, value)
	case string:
		if decl.bound == nil {
			return nil
		}
		if bound := *decl.bound; bound < len(value) {
			return fmt.Errorf(
				"string '%s' is over bounds, expecting %d but was %d", value,
				bound, len(value))
		}
		return nil
	case nil:
		if decl.nullable {
			return nil
		}
		return fmt.Errorf("expecting non-null string, found nil")
	}
}

// StructDecl describes a struct declaration.
type StructDecl struct {
	fidlir.Struct
	nullable bool
	schema   Schema
}

func (decl *StructDecl) IsNullable() bool {
	return decl.nullable
}

func (decl *StructDecl) Field(name string) (Declaration, bool) {
	for _, member := range decl.Members {
		if string(member.Name) == name {
			return decl.schema.lookupDeclByType(member.Type)
		}
	}
	return nil, false
}

// recordConforms is a helper function for implementing Declarations.conforms on
// types that expect a gidlir.Record value. It takes the kind ("struct", etc.),
// expected identifier, schema, and nullability, and returns the record or an
// error. It can also return (nil, nil) when value is nil and nullable is true.
func recordConforms(value interface{}, kind string, identifier fidlir.EncodedCompoundIdentifier, schema Schema, nullable bool) (*gidlir.Record, error) {
	switch value := value.(type) {
	default:
		return nil, fmt.Errorf("expecting %s, found %T (%v)", kind, value, value)
	case gidlir.Record:
		if actualIdentifier := schema.nameToIdentifier(value.Name); actualIdentifier != identifier {
			return nil, fmt.Errorf("expecting %s %s, found %s", kind, identifier, actualIdentifier)
		}
		return &value, nil
	case nil:
		if nullable {
			return nil, nil
		}
		return nil, fmt.Errorf("expecting non-null %s %s, found nil", kind, identifier)
	}
}

func (decl *StructDecl) conforms(value interface{}) error {
	record, err := recordConforms(value, "struct", decl.Name, decl.schema, decl.nullable)
	if err != nil {
		return err
	}
	if record == nil {
		return nil
	}
	for _, field := range record.Fields {
		if fieldDecl, ok := decl.Field(field.Key.Name); !ok {
			return fmt.Errorf("field %s: unknown", field.Key.Name)
		} else if err := fieldDecl.conforms(field.Value); err != nil {
			return fmt.Errorf("field %s: %s", field.Key.Name, err)
		}
	}
	return nil
}

// TableDecl describes a table declaration.
type TableDecl struct {
	NeverNullable
	fidlir.Table
	schema Schema
}

func (decl *TableDecl) Field(name string) (Declaration, bool) {
	for _, member := range decl.Members {
		if string(member.Name) == name {
			return decl.schema.lookupDeclByType(member.Type)
		}
	}
	return nil, false
}

func (decl *TableDecl) fieldByOrdinal(ordinal uint64) (Declaration, bool) {
	for _, member := range decl.Members {
		if uint64(member.Ordinal) == ordinal {
			return decl.schema.lookupDeclByType(member.Type)
		}
	}
	return nil, false
}

func (decl *TableDecl) conforms(value interface{}) error {
	record, err := recordConforms(value, "table", decl.Name, decl.schema, false)
	if err != nil {
		return err
	}
	if record == nil {
		panic("tables cannot be nullable")
	}
	for _, field := range record.Fields {
		if field.Key.IsUnknown() {
			if _, ok := decl.fieldByOrdinal(field.Key.UnknownOrdinal); ok {
				return fmt.Errorf("field name must be used rather than ordinal %d", field.Key.UnknownOrdinal)
			}
			continue
		}
		if fieldDecl, ok := decl.Field(field.Key.Name); !ok {
			return fmt.Errorf("field %s: unknown", field.Key.Name)
		} else if err := fieldDecl.conforms(field.Value); err != nil {
			return fmt.Errorf("field %s: %s", field.Key.Name, err)
		}
	}
	return nil
}

// UnionDecl describes a union declaration.
type UnionDecl struct {
	fidlir.Union
	nullable bool
	schema   Schema
}

func (decl *UnionDecl) IsNullable() bool {
	return decl.nullable
}

func (decl *UnionDecl) Field(name string) (Declaration, bool) {
	for _, member := range decl.Members {
		if string(member.Name) == name {
			return decl.schema.lookupDeclByType(member.Type)
		}
	}
	return nil, false
}

func (decl *UnionDecl) fieldByOrdinal(ordinal uint64) (Declaration, bool) {
	for _, member := range decl.Members {
		if uint64(member.Ordinal) == ordinal {
			return decl.schema.lookupDeclByType(member.Type)
		}
	}
	return nil, false
}

func (decl *UnionDecl) conforms(value interface{}) error {
	record, err := recordConforms(value, "union", decl.Name, decl.schema, decl.nullable)
	if err != nil {
		return err
	}
	if record == nil {
		return nil
	}
	if num := len(record.Fields); num != 1 {
		return fmt.Errorf("must have one field, found %d", num)
	}
	for _, field := range record.Fields {
		if field.Key.IsUnknown() {
			if _, ok := decl.fieldByOrdinal(field.Key.UnknownOrdinal); ok {
				return fmt.Errorf("field name must be used rather than ordinal %d", field.Key.UnknownOrdinal)
			}
			continue
		}
		if fieldDecl, ok := decl.Field(field.Key.Name); !ok {
			return fmt.Errorf("field %s: unknown", field.Key.Name)
		} else if err := fieldDecl.conforms(field.Value); err != nil {
			return fmt.Errorf("field %s: %s", field.Key.Name, err)
		}
	}
	return nil
}

type ArrayDecl struct {
	NeverNullable
	// The array has type `typ`, and it contains `typ.ElementType` elements.
	typ    fidlir.Type
	schema Schema
}

func (decl *ArrayDecl) Elem() Declaration {
	elemType := *decl.typ.ElementType
	elemDecl, ok := decl.schema.lookupDeclByType(elemType)
	if !ok {
		panic(fmt.Sprintf("array element type %v not found in schema", elemType))
	}
	return elemDecl
}

func (decl *ArrayDecl) Size() int {
	return *decl.typ.ElementCount
}

func (decl *ArrayDecl) conforms(untypedValue interface{}) error {
	switch value := untypedValue.(type) {
	default:
		return fmt.Errorf("expecting array, found %T (%v)", untypedValue, untypedValue)
	case []interface{}:
		if len(value) != decl.Size() {
			return fmt.Errorf("expecting %d elements, got %d", decl.Size(), len(value))
		}
		elemDecl := decl.Elem()
		for i, elem := range value {
			if err := elemDecl.conforms(elem); err != nil {
				return fmt.Errorf("[%d]: %s", i, err)
			}
		}
		return nil
	}
}

type VectorDecl struct {
	// The vector has type `typ`, and it contains `typ.ElementType` elements.
	typ    fidlir.Type
	schema Schema
}

func (decl *VectorDecl) IsNullable() bool {
	return decl.typ.Nullable
}

func (decl *VectorDecl) Elem() Declaration {
	elemType := *decl.typ.ElementType
	elemDecl, ok := decl.schema.lookupDeclByType(elemType)
	if !ok {
		panic(fmt.Sprintf("vector element type %v not found in schema", elemType))
	}
	return elemDecl
}

func (decl *VectorDecl) MaxSize() (int, bool) {
	if decl.typ.ElementCount != nil {
		return *decl.typ.ElementCount, true
	}
	return 0, false
}

func (decl *VectorDecl) conforms(untypedValue interface{}) error {
	switch value := untypedValue.(type) {
	default:
		return fmt.Errorf("expecting vector, found %T (%v)", untypedValue, untypedValue)
	case []interface{}:
		if maxSize, ok := decl.MaxSize(); ok && len(value) > maxSize {
			return fmt.Errorf("expecting at most %d elements, got %d", maxSize, len(value))
		}
		elemDecl := decl.Elem()
		for i, elem := range value {
			if err := elemDecl.conforms(elem); err != nil {
				return fmt.Errorf("[%d]: %s", i, err)
			}
		}
		return nil
	case nil:
		if decl.typ.Nullable {
			return nil
		}
		return fmt.Errorf("expecting non-nullable vector, got nil")
	}
}

// Schema is the GIDL-level concept of a FIDL library. It provides functions to
// lookup types and return the corresponding Declaration.
type Schema struct {
	library fidlir.EncodedLibraryIdentifier
	// Maps identifiers to *fidlir.Struct, *fidlir.Table, or *fidlir.Union.
	types map[fidlir.EncodedCompoundIdentifier]interface{}
}

// BuildSchema builds a Schema from a FIDL library.
// Note: The returned schema contains pointers into fidl.
func BuildSchema(fidl fidlir.Root) Schema {
	// TODO(fxb/43254): bits and enums
	total := len(fidl.Structs) + len(fidl.Tables) + len(fidl.Unions)
	types := make(map[fidlir.EncodedCompoundIdentifier]interface{}, total)
	// These loops must use fidl.Structs[i], fidl.Tables[i], etc. rather than
	// iterating `for i, decl := ...` and using &decl, because that would store
	// the same local variable address in every entry.
	for i := range fidl.Structs {
		decl := &fidl.Structs[i]
		types[decl.Name] = decl
	}
	for i := range fidl.Tables {
		decl := &fidl.Tables[i]
		types[decl.Name] = decl
	}
	for i := range fidl.Unions {
		decl := &fidl.Unions[i]
		types[decl.Name] = decl
	}
	return Schema{library: fidl.Name, types: types}
}

// ExtractDeclaration extract the top-level declaration for the provided value,
// and ensures the value conforms to the schema.
func (s Schema) ExtractDeclaration(value interface{}) (*StructDecl, error) {
	decl, err := s.ExtractDeclarationUnsafe(value)
	if err != nil {
		return nil, err
	}
	if err := decl.conforms(value); err != nil {
		return nil, fmt.Errorf("value %v failed to conform to declaration (type %T): %v", value, decl, err)
	}
	return decl, nil
}

// ExtractDeclarationUnsafe extracts the top-level declaration for the provided
// value, but does not ensure the value conforms to the schema. This is used in
// cases where conformance is too strict (e.g. failure cases).
func (s Schema) ExtractDeclarationUnsafe(value interface{}) (*StructDecl, error) {
	switch value := value.(type) {
	case gidlir.Record:
		return s.ExtractDeclarationByName(value.Name)
	default:
		return nil, fmt.Errorf("top-level message must be a struct; got %s (%T)", value, value)
	}
}

// ExtractDeclarationByName extracts the top-level declaration for the given
// type name. This is used in cases where only the type name is provided in the
// test (e.g. decoding-only tests).
func (s Schema) ExtractDeclarationByName(name string) (*StructDecl, error) {
	decl, ok := s.lookupDeclByName(name, false)
	if !ok {
		return nil, fmt.Errorf("unknown declaration %s", name)
	}
	structDecl, ok := decl.(*StructDecl)
	if !ok {
		return nil, fmt.Errorf("top-level message must be a struct; got %s (%T)", name, decl)
	}
	return structDecl, nil
}

func (s Schema) lookupDeclByName(name string, nullable bool) (Declaration, bool) {
	return s.lookupDeclByECI(s.nameToIdentifier(name), nullable)
}

func (s Schema) nameToIdentifier(name string) fidlir.EncodedCompoundIdentifier {
	return fidlir.EncodedCompoundIdentifier(fmt.Sprintf("%s/%s", s.library, name))
}

func (s Schema) lookupDeclByECI(eci fidlir.EncodedCompoundIdentifier, nullable bool) (Declaration, bool) {
	typ, ok := s.types[eci]
	if !ok {
		return nil, false
	}
	switch typ := typ.(type) {
	case *fidlir.Struct:
		return &StructDecl{
			Struct:   *typ,
			nullable: nullable,
			schema:   s,
		}, true
	case *fidlir.Table:
		if nullable {
			panic(fmt.Sprintf("nullable table %s is not allowed", typ.Name))
		}
		return &TableDecl{
			Table:  *typ,
			schema: s,
		}, true
	case *fidlir.Union:
		return &UnionDecl{
			Union:    *typ,
			nullable: nullable,
			schema:   s,
		}, true
	}
	// TODO(fxb/43254): bits and enums
	return nil, false
}

func (s Schema) lookupDeclByType(typ fidlir.Type) (Declaration, bool) {
	switch typ.Kind {
	case fidlir.StringType:
		return &StringDecl{
			bound:    typ.ElementCount,
			nullable: typ.Nullable,
		}, true
	case fidlir.PrimitiveType:
		switch typ.PrimitiveSubtype {
		case fidlir.Bool:
			return &BoolDecl{}, true
		case fidlir.Int8:
			return &IntegerDecl{subtype: typ.PrimitiveSubtype, lower: math.MinInt8, upper: math.MaxInt8}, true
		case fidlir.Int16:
			return &IntegerDecl{subtype: typ.PrimitiveSubtype, lower: math.MinInt16, upper: math.MaxInt16}, true
		case fidlir.Int32:
			return &IntegerDecl{subtype: typ.PrimitiveSubtype, lower: math.MinInt32, upper: math.MaxInt32}, true
		case fidlir.Int64:
			return &IntegerDecl{subtype: typ.PrimitiveSubtype, lower: math.MinInt64, upper: math.MaxInt64}, true
		case fidlir.Uint8:
			return &IntegerDecl{subtype: typ.PrimitiveSubtype, lower: 0, upper: math.MaxUint8}, true
		case fidlir.Uint16:
			return &IntegerDecl{subtype: typ.PrimitiveSubtype, lower: 0, upper: math.MaxUint16}, true
		case fidlir.Uint32:
			return &IntegerDecl{subtype: typ.PrimitiveSubtype, lower: 0, upper: math.MaxUint32}, true
		case fidlir.Uint64:
			return &IntegerDecl{subtype: typ.PrimitiveSubtype, lower: 0, upper: math.MaxUint64}, true
		case fidlir.Float32:
			return &FloatDecl{subtype: typ.PrimitiveSubtype}, true
		case fidlir.Float64:
			return &FloatDecl{subtype: typ.PrimitiveSubtype}, true
		default:
			panic(fmt.Sprintf("unsupported primitive subtype: %s", typ.PrimitiveSubtype))
		}
	case fidlir.IdentifierType:
		return s.lookupDeclByECI(typ.Identifier, typ.Nullable)
	case fidlir.ArrayType:
		return &ArrayDecl{schema: s, typ: typ}, true
	case fidlir.VectorType:
		return &VectorDecl{schema: s, typ: typ}, true
	default:
		// TODO(fxb/36441): HandleType, RequestType
		panic("not implemented")
	}
}
