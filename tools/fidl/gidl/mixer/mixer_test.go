// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
package mixer

import (
	"encoding/json"
	"fmt"
	"io/ioutil"
	"math"
	"os"
	"path/filepath"
	"strings"
	"testing"

	fidlir "fidl/compiler/backend/types"
	gidlir "gidl/ir"
)

var testDataPath = func() string {
	path, err := filepath.Abs(os.Args[0])
	if err != nil {
		panic(err)
	}
	return filepath.Join(filepath.Dir(path), "test_data", "gidl")
}()

var fidlRoot = func() fidlir.Root {
	path := filepath.Join(testDataPath, "mixer.test.fidl.json")
	bytes, err := ioutil.ReadFile(path)
	if err != nil {
		panic(err)
	}
	var root fidlir.Root
	err = json.Unmarshal(bytes, &root)
	if err != nil {
		panic(fmt.Sprintf("failed to unmarshal %s: %s", path, err))
	}
	return root
}()

// checkStruct is a helper function to test the Declaration for a struct.
func checkStruct(t *testing.T, decl Declaration, expectedName string, expectedNullable bool) {
	t.Helper()
	structDecl, ok := decl.(*StructDecl)
	if !ok {
		t.Fatalf("expected StructDecl, got %T\n\ndecl: %#v", decl, decl)
	}
	name := structDecl.Name.Parts()
	if name.Name != fidlir.Identifier(expectedName) {
		t.Errorf("expected name to be %s, got %s\n\ndecl: %#v", expectedName, name.Name, decl)
	}
	if structDecl.nullable != expectedNullable {
		t.Errorf("expected nullable to be %v, got %v\n\ndecl: %#v",
			expectedNullable, structDecl.nullable, decl)
	}
}

func TestLookupDeclByNameNonNullable(t *testing.T) {
	decl, ok := schema(fidlRoot).LookupDeclByName("ExampleStruct", false)
	if !ok {
		t.Fatalf("LookupDeclByName failed")
	}
	checkStruct(t, decl, "ExampleStruct", false)
}

func TestLookupDeclByNameNullable(t *testing.T) {
	decl, ok := schema(fidlRoot).LookupDeclByName("ExampleStruct", true)
	if !ok {
		t.Fatalf("LookupDeclByName failed")
	}
	checkStruct(t, decl, "ExampleStruct", true)
}

func TestLookupDeclByNameFailure(t *testing.T) {
	decl, ok := schema(fidlRoot).LookupDeclByName("ThisIsNotAStruct", false)
	if ok {
		t.Fatalf("LookupDeclByName unexpectedly succeeded: %#v", decl)
	}
}

func TestLookupDeclByTypeSuccess(t *testing.T) {
	typ := fidlir.Type{
		Kind:             fidlir.PrimitiveType,
		PrimitiveSubtype: fidlir.Bool,
	}
	decl, ok := schema(fidlRoot).LookupDeclByType(typ)
	if !ok {
		t.Fatalf("LookupDeclByType failed")
	}
	if _, ok := decl.(*BoolDecl); !ok {
		t.Fatalf("expected BoolDecl, got %T\n\ndecl: %#v", decl, decl)
	}
}

func TestExtractDeclarationSuccess(t *testing.T) {
	value := gidlir.Object{
		Name: "ExampleStruct",
		Fields: []gidlir.Field{
			{Key: gidlir.FieldKey{Name: "s"}, Value: "foo"},
		},
	}
	decl, err := ExtractDeclaration(value, fidlRoot)
	if err != nil {
		t.Fatalf("ExtractDeclaration failed: %s", err)
	}
	checkStruct(t, decl, "ExampleStruct", false)
}

func TestExtractDeclarationNotDefined(t *testing.T) {
	value := gidlir.Object{
		Name:   "ThisIsNotAStruct",
		Fields: []gidlir.Field{},
	}
	decl, err := ExtractDeclaration(value, fidlRoot)
	if err == nil {
		t.Fatalf("ExtractDeclaration unexpectedly succeeded: %#v", decl)
	}
	if !strings.Contains(err.Error(), "unknown") {
		t.Fatalf("expected err to contain 'unknown', got '%s'", err)
	}
}

func TestExtractDeclarationDoesNotConform(t *testing.T) {
	value := gidlir.Object{
		Name: "ExampleStruct",
		Fields: []gidlir.Field{
			{Key: gidlir.FieldKey{Name: "ThisIsNotAField"}, Value: "foo"},
		},
	}
	decl, err := ExtractDeclaration(value, fidlRoot)
	if err == nil {
		t.Fatalf("ExtractDeclaration unexpectedly succeeded: %#v", decl)
	}
	if !strings.Contains(err.Error(), "conform") {
		t.Fatalf("expected err to contain 'conform', got '%s'", err)
	}
}

func TestExtractDeclarationUnsafeSuccess(t *testing.T) {
	value := gidlir.Object{
		Name: "ExampleStruct",
		Fields: []gidlir.Field{
			{Key: gidlir.FieldKey{Name: "ThisIsNotAField"}, Value: "foo"},
		},
	}
	decl, err := ExtractDeclarationUnsafe(value, fidlRoot)
	if err != nil {
		t.Fatalf("ExtractDeclarationUnsafe failed: %s", err)
	}
	checkStruct(t, decl, "ExampleStruct", false)
}

// conformTest describes a test case for the Declaration.conforms method.
type conformTest interface {
	shouldConform() bool
	value() interface{}
}

type conformOk struct{ val interface{} }
type conformFail struct{ val interface{} }

func (c conformOk) shouldConform() bool { return true }
func (c conformOk) value() interface{}  { return c.val }

func (c conformFail) shouldConform() bool { return false }
func (c conformFail) value() interface{}  { return c.val }

// checkConforms is a helper function to test the Declaration.conforms method.
func checkConforms(t *testing.T, decl Declaration, tests []conformTest) {
	t.Helper()
	for _, test := range tests {
		value, expected := test.value(), test.shouldConform()
		if err := decl.conforms(value); (err == nil) != expected {
			if expected {
				t.Errorf(
					"value failed to conform to declaration\n\nvalue: %#v\n\nerr: %s\n\ndecl: %#v",
					value, err, decl)
			} else {
				t.Errorf(
					"value unexpectedly conformed to declaration\n\nvalue: %#v\n\ndecl: %#v",
					value, decl)
			}
		}
	}
}

func TestBoolDeclConforms(t *testing.T) {
	checkConforms(t,
		&BoolDecl{},
		[]conformTest{
			conformOk{false},
			conformOk{true},
			conformFail{nil},
			conformFail{"foo"},
			conformFail{42},
			conformFail{int64(42)},
		},
	)
}

func TestNumberDeclConforms(t *testing.T) {
	checkConforms(t,
		&NumberDecl{Typ: fidlir.Uint8, lower: 0, upper: 255},
		[]conformTest{
			conformOk{uint64(0)},
			conformOk{uint64(128)},
			conformOk{uint64(255)},
			conformFail{int64(-1)},
			conformFail{int64(256)},
			conformFail{nil},
			// Must be uint64 or int64, not any integer type.
			conformFail{0},
			conformFail{uint(0)},
			conformFail{int8(0)},
			conformFail{uint8(0)},
			conformFail{"foo"},
			conformFail{1.5},
		},
	)
	checkConforms(t,
		&NumberDecl{Typ: fidlir.Int64, lower: -5, upper: 10},
		[]conformTest{
			conformOk{int64(-5)},
			conformOk{int64(10)},
			conformFail{int64(-6)},
			conformFail{int64(11)},
		},
	)
}

func TestFloatDeclConforms(t *testing.T) {
	tests := []conformTest{
		conformOk{0.0},
		conformOk{1.5},
		conformOk{-1.0},
		conformFail{nil},
		// Must be float64, not float32.
		conformFail{float32(0.0)},
		conformFail{0},
		conformFail{"foo"},
		// TODO(fxb/43020): Allow these once each backend supports them.
		conformFail{math.Inf(1)},
		conformFail{math.Inf(-1)},
		conformFail{math.NaN()},
	}
	checkConforms(t, &FloatDecl{Typ: fidlir.Float32}, tests)
	checkConforms(t, &FloatDecl{Typ: fidlir.Float64}, tests)
}

func TestStringDeclConforms(t *testing.T) {
	checkConforms(t,
		&StringDecl{bound: nil, nullable: false},
		[]conformTest{
			conformOk{""},
			conformOk{"the quick brown fox"},
			conformFail{nil},
			conformFail{0},
		},
	)
	checkConforms(t,
		&StringDecl{bound: nil, nullable: true},
		[]conformTest{
			conformOk{"foo"},
			conformOk{nil},
			conformFail{0},
		},
	)
	two := 2
	checkConforms(t,
		&StringDecl{bound: &two, nullable: false},
		[]conformTest{
			conformOk{""},
			conformOk{"1"},
			conformOk{"12"},
			conformFail{"123"},
			conformFail{"the quick brown fox"},
		},
	)
}

func TestStructDeclConformsNonNullable(t *testing.T) {
	decl, ok := schema(fidlRoot).LookupDeclByName("ExampleStruct", false)
	if !ok {
		t.Fatalf("LookupDeclByName failed")
	}
	structDecl := decl.(*StructDecl)
	checkConforms(t,
		structDecl,
		[]conformTest{
			conformOk{gidlir.Object{
				Name: "ExampleStruct",
				Fields: []gidlir.Field{
					{Key: gidlir.FieldKey{Name: "s"}, Value: "foo"},
				},
			}},
			conformFail{gidlir.Object{
				Name: "ExampleStruct",
				Fields: []gidlir.Field{
					{Key: gidlir.FieldKey{Name: "DefinitelyNotS"}, Value: "foo"},
				},
			}},
			conformFail{gidlir.Object{
				Name: "DefinitelyNotExampleStruct",
				Fields: []gidlir.Field{
					{Key: gidlir.FieldKey{Name: "s"}, Value: "foo"},
				},
			}},
			conformFail{nil},
			conformFail{"foo"},
			conformFail{0},
		},
	)
}

func TestStructDeclConformsNullable(t *testing.T) {
	decl, ok := schema(fidlRoot).LookupDeclByName("ExampleStruct", true)
	if !ok {
		t.Fatalf("LookupDeclByName failed")
	}
	structDecl := decl.(*StructDecl)
	checkConforms(t,
		structDecl,
		[]conformTest{
			conformOk{gidlir.Object{
				Name: "ExampleStruct",
				Fields: []gidlir.Field{
					{Key: gidlir.FieldKey{Name: "s"}, Value: "foo"},
				},
			}},
			conformOk{nil},
		},
	)
}

func TestTableDeclConforms(t *testing.T) {
	decl, ok := schema(fidlRoot).LookupDeclByName("ExampleTable", false)
	if !ok {
		t.Fatalf("LookupDeclByName failed")
	}
	tableDecl := decl.(*TableDecl)
	checkConforms(t,
		tableDecl,
		[]conformTest{
			conformOk{gidlir.Object{
				Name: "ExampleTable",
				Fields: []gidlir.Field{
					{Key: gidlir.FieldKey{Name: "t"}, Value: "foo"},
				},
			}},
			conformFail{gidlir.Object{
				Name: "ExampleTable",
				Fields: []gidlir.Field{
					{Key: gidlir.FieldKey{Name: "DefinitelyNotT"}, Value: "foo"},
				},
			}},
			conformFail{gidlir.Object{
				Name: "DefinitelyNotExampleTable",
				Fields: []gidlir.Field{
					{Key: gidlir.FieldKey{Name: "t"}, Value: "foo"},
				},
			}},
			conformFail{nil},
			conformFail{"foo"},
			conformFail{0},
		},
	)
}

func TestUnionConvertsToXUnionDecl(t *testing.T) {
	decl, ok := schema(fidlRoot).LookupDeclByName("ExampleUnion", false)
	if !ok {
		t.Fatalf("LookupDeclByName failed")
	}
	if _, ok := decl.(*XUnionDecl); !ok {
		t.Fatalf("expected XUnionDecl, got %T", decl)
	}
}

func TestXUnionDeclConformsNonNullable(t *testing.T) {
	decl, ok := schema(fidlRoot).LookupDeclByName("ExampleXUnion", false)
	if !ok {
		t.Fatalf("LookupDeclByName failed")
	}
	xunionDecl := decl.(*XUnionDecl)
	checkConforms(t,
		xunionDecl,
		[]conformTest{
			conformOk{gidlir.Object{
				Name: "ExampleXUnion",
				Fields: []gidlir.Field{
					{Key: gidlir.FieldKey{Name: "x"}, Value: "foo"},
				},
			}},
			conformFail{gidlir.Object{
				Name: "ExampleXUnion",
				Fields: []gidlir.Field{
					{Key: gidlir.FieldKey{Name: "DefinitelyNotX"}, Value: "foo"},
				},
			}},
			conformFail{gidlir.Object{
				Name: "DefinitelyNotExampleXUnion",
				Fields: []gidlir.Field{
					{Key: gidlir.FieldKey{Name: "x"}, Value: "foo"},
				},
			}},
			conformFail{nil},
			conformFail{"foo"},
			conformFail{0},
		},
	)
}

func TestXUnionDeclConformsNullable(t *testing.T) {
	decl, ok := schema(fidlRoot).LookupDeclByName("ExampleXUnion", true)
	if !ok {
		t.Fatalf("LookupDeclByName failed")
	}
	xunionDecl := decl.(*XUnionDecl)
	checkConforms(t,
		xunionDecl,
		[]conformTest{
			conformOk{gidlir.Object{
				Name: "ExampleXUnion",
				Fields: []gidlir.Field{
					{Key: gidlir.FieldKey{Name: "x"}, Value: "foo"},
				},
			}},
			conformOk{nil},
		},
	)
}

func TestArrayDeclConforms(t *testing.T) {
	two := 2
	checkConforms(t,
		&ArrayDecl{
			schema: schema(fidlRoot),
			typ: fidlir.Type{
				Kind:         fidlir.ArrayType,
				ElementCount: &two,
				ElementType: &fidlir.Type{
					Kind:             fidlir.PrimitiveType,
					PrimitiveSubtype: fidlir.Uint8,
				},
			},
		},
		[]conformTest{
			conformOk{[]interface{}{uint64(1), uint64(2)}},
			conformFail{[]interface{}{}},
			conformFail{[]interface{}{uint64(1)}},
			conformFail{[]interface{}{uint64(1), uint64(1), uint64(1)}},
			conformFail{[]interface{}{"a", "b"}},
			conformFail{[]interface{}{nil, nil}},
		},
	)
}

func TestVectorDeclConforms(t *testing.T) {
	two := 2
	checkConforms(t,
		&VectorDecl{
			schema: schema(fidlRoot),
			typ: fidlir.Type{
				Kind:         fidlir.VectorType,
				ElementCount: &two,
				ElementType: &fidlir.Type{
					Kind:             fidlir.PrimitiveType,
					PrimitiveSubtype: fidlir.Uint8,
				},
			},
		},
		[]conformTest{
			conformOk{[]interface{}{}},
			conformOk{[]interface{}{uint64(1)}},
			conformOk{[]interface{}{uint64(1), uint64(2)}},
			conformFail{[]interface{}{uint64(1), uint64(1), uint64(1)}},
			conformFail{[]interface{}{"a", "b"}},
			conformFail{[]interface{}{nil, nil}},
		},
	)
}

type visitor struct {
	visited string
}

func (v *visitor) OnBool(bool)                                { v.visited = "Bool" }
func (v *visitor) OnInt64(int64, fidlir.PrimitiveSubtype)     { v.visited = "Int64" }
func (v *visitor) OnUint64(uint64, fidlir.PrimitiveSubtype)   { v.visited = "Uint64" }
func (v *visitor) OnFloat64(float64, fidlir.PrimitiveSubtype) { v.visited = "Float64" }
func (v *visitor) OnString(string, *StringDecl)               { v.visited = "String" }
func (v *visitor) OnStruct(gidlir.Object, *StructDecl)        { v.visited = "Struct" }
func (v *visitor) OnTable(gidlir.Object, *TableDecl)          { v.visited = "Table" }
func (v *visitor) OnXUnion(gidlir.Object, *XUnionDecl)        { v.visited = "XUnion" }
func (v *visitor) OnArray([]interface{}, *ArrayDecl)          { v.visited = "Array" }
func (v *visitor) OnVector([]interface{}, *VectorDecl)        { v.visited = "Vector" }
func (v *visitor) OnNull(Declaration)                         { v.visited = "Null" }

func TestVisit(t *testing.T) {
	tests := []struct {
		value    interface{}
		decl     Declaration
		expected string
	}{
		{false, &BoolDecl{}, "Bool"},
		{int64(1), &NumberDecl{Typ: fidlir.Int8}, "Int64"},
		{uint64(1), &NumberDecl{Typ: fidlir.Uint8}, "Uint64"},
		{1.23, &FloatDecl{Typ: fidlir.Float32}, "Float64"},
		{"foo", &StringDecl{}, "String"},
		{nil, &StringDecl{nullable: true}, "Null"},
		// These values and decls are not fully initialized, but for the
		// purposes of Visit() it should not matter.
		{gidlir.Object{}, &StructDecl{}, "Struct"},
		{gidlir.Object{}, &TableDecl{}, "Table"},
		{gidlir.Object{}, &XUnionDecl{}, "XUnion"},
	}
	for _, test := range tests {
		var v visitor
		Visit(&v, test.value, test.decl)
		if v.visited != test.expected {
			t.Errorf("expected dispatch to %q, got %q\n\nvalue: %#v\n\ndecl:%#v",
				test.expected, v.visited, test.value, test.decl)
		}
	}
}
