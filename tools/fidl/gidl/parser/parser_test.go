// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
package parser

import (
	"fmt"
	"gidl/ir"
	"strings"
	"testing"

	"github.com/google/go-cmp/cmp"
)

func TestParseValues(t *testing.T) {
	type testCase struct {
		gidl          string
		expectedValue interface{}
	}
	testCases := []testCase{
		{gidl: `1`, expectedValue: uint64(1)},
		{gidl: `-78`, expectedValue: int64(-78)},
		{gidl: `3.14`, expectedValue: float64(3.14)},
		{gidl: `-3.14`, expectedValue: float64(-3.14)},
		{gidl: `"hello"`, expectedValue: "hello"},
		{gidl: `"\x00"`, expectedValue: "\x00"},
		{gidl: `"\""`, expectedValue: "\""},
		{gidl: `true`, expectedValue: true},
		{gidl: `null`, expectedValue: nil},
		{gidl: `SomeRecord {}`, expectedValue: ir.Record{
			Name: "SomeRecord",
		}},
		{gidl: `SomeRecord { the_field: 5, }`, expectedValue: ir.Record{
			Name: "SomeRecord",
			Fields: []ir.Field{
				{
					Key: ir.FieldKey{
						Name: "the_field",
					},
					Value: uint64(5),
				},
			},
		}},
		{gidl: `SomeRecord { the_field: null, }`, expectedValue: ir.Record{
			Name: "SomeRecord",
			Fields: []ir.Field{
				{
					Key: ir.FieldKey{
						Name: "the_field",
					},
					Value: nil,
				},
			},
		}},
		{gidl: `SomeRecord { 0x01020304: 5, }`, expectedValue: ir.Record{
			Name: "SomeRecord",
			Fields: []ir.Field{
				{
					Key: ir.FieldKey{
						UnknownOrdinal: 0x01020304,
					},
					Value: uint64(5),
				},
			},
		}},
		{gidl: `SomeRecord { f1: 0x01, }`, expectedValue: ir.Record{
			Name: "SomeRecord",
			Fields: []ir.Field{
				{
					Key: ir.FieldKey{
						Name: "f1",
					},
					Value: uint64(1),
				},
			},
		}},
		{gidl: `SomeRecord {
			the_field: SomeNestedRecord {
				foo: 5,
				bar: 7,
			},
		}`, expectedValue: ir.Record{
			Name: "SomeRecord",
			Fields: []ir.Field{
				{
					Key: ir.FieldKey{
						Name: "the_field",
					},
					Value: ir.Record{
						Name: "SomeNestedRecord",
						Fields: []ir.Field{
							{
								Key: ir.FieldKey{
									Name: "foo",
								},
								Value: uint64(5),
							},
							{
								Key: ir.FieldKey{
									Name: "bar",
								},
								Value: uint64(7),
							},
						},
					},
				},
			},
		}},
		{gidl: `[]`, expectedValue: []interface{}(nil)},
		{gidl: `[1,]`, expectedValue: []interface{}{uint64(1)}},
		{gidl: `[1,"hello",true,]`, expectedValue: []interface{}{uint64(1), "hello", true}},
		{gidl: `[null,]`, expectedValue: []interface{}{nil}},
	}
	for _, tc := range testCases {
		t.Run(tc.gidl, func(t *testing.T) {
			p := NewParser("", strings.NewReader(tc.gidl), []string{})
			value, err := p.parseValue()
			checkMatch(t, value, tc.expectedValue, err)
		})
	}
}

func TestFailsParseValues(t *testing.T) {
	type testCase struct {
		gidl                string
		expectedErrorSubstr string
	}
	testCases := []testCase{
		{gidl: `"`, expectedErrorSubstr: "improperly escaped string"},
		{gidl: `"\xwrong"`, expectedErrorSubstr: "improperly escaped string"},
	}
	for _, tc := range testCases {
		t.Run(tc.gidl, func(t *testing.T) {
			p := NewParser("", strings.NewReader(tc.gidl), []string{})
			_, err := p.parseValue()
			checkFailure(t, err, tc.expectedErrorSubstr)
		})
	}
}

func TestParseBytes(t *testing.T) {
	type testCase struct {
		gidl          string
		expectedValue []ir.Encoding
	}
	testCases := []testCase{
		// empty
		{
			gidl: `{ v1 = [] }`,
			expectedValue: []ir.Encoding{
				{
					WireFormat: ir.V1WireFormat,
					Bytes:      nil,
				},
			},
		},
		// base 10
		{
			gidl: `{ v1 = [1, 2, 3] }`,
			expectedValue: []ir.Encoding{
				{
					WireFormat: ir.V1WireFormat,
					Bytes:      []byte{1, 2, 3},
				},
			},
		},
		// base 16
		{
			gidl: `{ v1 = [0x0, 0xff, 0xA, 0x0a, 7] }`,
			expectedValue: []ir.Encoding{
				{
					WireFormat: ir.V1WireFormat,
					Bytes:      []byte{0, 255, 10, 10, 7},
				},
			},
		},
		// character codes
		{
			gidl: `{ v1 = ['h', 'e', 'l', 'l', 'o'] }`,
			expectedValue: []ir.Encoding{
				{
					WireFormat: ir.V1WireFormat,
					Bytes:      []byte{'h', 'e', 'l', 'l', 'o'},
				},
			},
		},
		// positive number
		{
			gidl: `{ v1 = [num(2147483647):4] }`,
			expectedValue: []ir.Encoding{
				{
					WireFormat: ir.V1WireFormat,
					Bytes:      []byte{0xff, 0xff, 0xff, 0x7f},
				},
			},
		},
		// negative number
		{
			gidl: `{ v1 = [num(-32768):2] }`,
			expectedValue: []ir.Encoding{
				{
					WireFormat: ir.V1WireFormat,
					Bytes:      []byte{0x00, 0x80},
				},
			},
		},
		// padding
		{
			gidl: `{ v1 = [padding:3] }`,
			expectedValue: []ir.Encoding{
				{
					WireFormat: ir.V1WireFormat,
					Bytes:      []byte{0, 0, 0},
				},
			},
		},
		// repeat a byte
		{
			gidl: `{ v1 = [repeat(0x33):3] }`,
			expectedValue: []ir.Encoding{
				{
					WireFormat: ir.V1WireFormat,
					Bytes:      []byte{0x33, 0x33, 0x33},
				},
			},
		},
		// multiple byte generators in same list
		{
			gidl: `{ v1 = [num(127):2, repeat(0x33):3] }`,
			expectedValue: []ir.Encoding{
				{
					WireFormat: ir.V1WireFormat,
					Bytes:      []byte{0x7f, 0x00, 0x33, 0x33, 0x33},
				},
			},
		},
		// mix plain bytes, characters, and generators
		{
			gidl: `{ v1 = [num(127):2, 255, padding:1, 'A'] }`,
			expectedValue: []ir.Encoding{
				{
					WireFormat: ir.V1WireFormat,
					Bytes:      []byte{0x7f, 0x00, 0xff, 0x00, 'A'},
				},
			},
		},
		// trailing comma allowed
		{
			gidl: `{ v1 = [1,2,] }`,
			expectedValue: []ir.Encoding{
				{
					WireFormat: ir.V1WireFormat,
					Bytes:      []byte{1, 2},
				},
			},
		},
		// multiple wire formats, same bytes (empty), old first
		{
			gidl: `{ old, v1 = [] }`,
			expectedValue: []ir.Encoding{
				{
					WireFormat: ir.OldWireFormat,
					Bytes:      nil,
				},
				{
					WireFormat: ir.V1WireFormat,
					Bytes:      nil,
				},
			},
		},
		// multiple wire formats, same bytes (empty), v1 first
		{
			gidl: `{ v1, old = [] }`,
			expectedValue: []ir.Encoding{
				{
					WireFormat: ir.V1WireFormat,
					Bytes:      nil,
				},
				{
					WireFormat: ir.OldWireFormat,
					Bytes:      nil,
				},
			},
		},
		// multiple wire formats, same bytes (non-empty)
		{
			gidl: `{ old, v1 = [1, 2, 3] }`,
			expectedValue: []ir.Encoding{
				{
					WireFormat: ir.OldWireFormat,
					Bytes:      []byte{1, 2, 3},
				},
				{
					WireFormat: ir.V1WireFormat,
					Bytes:      []byte{1, 2, 3},
				},
			},
		},
		// multiple wire formats, different bytes
		{
			gidl: `{
				old = [1, 2, 3],
				v1 = [repeat(4):3],
			}`,
			expectedValue: []ir.Encoding{
				{
					WireFormat: ir.OldWireFormat,
					Bytes:      []byte{1, 2, 3},
				},
				{
					WireFormat: ir.V1WireFormat,
					Bytes:      []byte{4, 4, 4},
				},
			},
		},
	}
	for _, tc := range testCases {
		p := NewParser("", strings.NewReader(tc.gidl), []string{})
		value, err := p.parseByteSection()
		t.Run(tc.gidl, func(t *testing.T) {
			checkMatch(t, value, tc.expectedValue, err)
		})
	}
}

func TestParseBytesFailures(t *testing.T) {
	type testCase struct {
		gidl         string
		errSubstring string
	}
	testCases := []testCase{
		{
			gidl:         `{}`,
			errSubstring: "no bytes",
		},
		{
			gidl:         `{ v1 = [PADDING:0] }`,
			errSubstring: "invalid byte syntax",
		},
		{
			gidl:         `{ v1 = [thisisnotagenerator(1):0] }`,
			errSubstring: "invalid byte syntax",
		},
		{
			gidl:         `{ v1 = [padding:0] }`,
			errSubstring: "non-zero",
		},
		{
			gidl:         `{ v1 = [num(65536):2] }`,
			errSubstring: "exceeds byte size",
		},
		{
			gidl:         `{ v1 = [num(-32769):2] }`,
			errSubstring: "exceeds byte size",
		},
		{
			gidl:         `{ v1, v1 = [] }`,
			errSubstring: "duplicate wire format",
		},
		{
			gidl:         `{ v1 = [], old, v1 = [] }`,
			errSubstring: "duplicate wire format",
		},
	}
	for _, tc := range testCases {
		p := NewParser("", strings.NewReader(tc.gidl), []string{})
		_, err := p.parseByteSection()
		t.Run(tc.gidl, func(t *testing.T) {
			if err == nil {
				t.Fatalf("error was expected, but no error was returned")
			}
			if !strings.Contains(err.Error(), tc.errSubstring) {
				t.Errorf("expected error containing %q, but got %q", tc.errSubstring, err.Error())
			}
		})
	}
}

func TestParseSuccessCase(t *testing.T) {
	gidl := `
	success("OneStringOfMaxLengthFive-empty") {
		value = OneStringOfMaxLengthFive {
			first: "four",
		},
		bytes = {
			v1 = [
				0, 0, 0, 0, 0, 0, 0, 0, // length
				255, 255, 255, 255, 255, 255, 255, 255, // alloc present
			],
		},
	}`
	all, err := parse(gidl)
	expectedAll := ir.All{
		EncodeSuccess: []ir.EncodeSuccess{{
			Name: "OneStringOfMaxLengthFive-empty",
			Value: ir.Record{
				Name: "OneStringOfMaxLengthFive",
				Fields: []ir.Field{
					{
						Key: ir.FieldKey{
							Name: "first",
						},
						Value: "four",
					},
				},
			},
			Encodings: []ir.Encoding{{
				WireFormat: ir.V1WireFormat,
				Bytes: []byte{
					0, 0, 0, 0, 0, 0, 0, 0, // length
					255, 255, 255, 255, 255, 255, 255, 255, // alloc present
				},
			}},
		}},
		DecodeSuccess: []ir.DecodeSuccess{{
			Name: "OneStringOfMaxLengthFive-empty",
			Value: ir.Record{
				Name: "OneStringOfMaxLengthFive",
				Fields: []ir.Field{
					{
						Key: ir.FieldKey{
							Name: "first",
						},
						Value: "four",
					},
				},
			},
			Encodings: []ir.Encoding{{
				WireFormat: ir.V1WireFormat,
				Bytes: []byte{
					0, 0, 0, 0, 0, 0, 0, 0, // length
					255, 255, 255, 255, 255, 255, 255, 255, // alloc present
				},
			}},
		}},
	}
	checkMatch(t, all, expectedAll, err)
}

func TestParseEncodeSuccessCase(t *testing.T) {
	gidl := `
	encode_success("OneStringOfMaxLengthFive-empty") {
		value = OneStringOfMaxLengthFive {
			first: "four",
		},
		bytes = {
			v1 = [
				0, 0, 0, 0, 0, 0, 0, 0, // length
				255, 255, 255, 255, 255, 255, 255, 255, // alloc present
			],
		},
	}`
	all, err := parse(gidl)
	expectedAll := ir.All{
		EncodeSuccess: []ir.EncodeSuccess{{
			Name: "OneStringOfMaxLengthFive-empty",
			Value: ir.Record{
				Name: "OneStringOfMaxLengthFive",
				Fields: []ir.Field{
					{
						Key: ir.FieldKey{
							Name: "first",
						},
						Value: "four",
					},
				},
			},
			Encodings: []ir.Encoding{{
				WireFormat: ir.V1WireFormat,
				Bytes: []byte{
					0, 0, 0, 0, 0, 0, 0, 0, // length
					255, 255, 255, 255, 255, 255, 255, 255, // alloc present
				},
			}},
		}},
	}
	checkMatch(t, all, expectedAll, err)
}

func TestParseDecodeSuccessCase(t *testing.T) {
	gidl := `
	decode_success("OneStringOfMaxLengthFive-empty") {
		value = OneStringOfMaxLengthFive {
			first: "four",
		},
		bytes = {
			v1 = [
				0, 0, 0, 0, 0, 0, 0, 0, // length
				255, 255, 255, 255, 255, 255, 255, 255, // alloc present
			],
		},
	}`
	all, err := parse(gidl)
	expectedAll := ir.All{
		DecodeSuccess: []ir.DecodeSuccess{{
			Name: "OneStringOfMaxLengthFive-empty",
			Value: ir.Record{
				Name: "OneStringOfMaxLengthFive",
				Fields: []ir.Field{
					{
						Key: ir.FieldKey{
							Name: "first",
						},
						Value: "four",
					},
				},
			},
			Encodings: []ir.Encoding{{
				WireFormat: ir.V1WireFormat,
				Bytes: []byte{
					0, 0, 0, 0, 0, 0, 0, 0, // length
					255, 255, 255, 255, 255, 255, 255, 255, // alloc present
				},
			}},
		}},
	}
	checkMatch(t, all, expectedAll, err)
}
func TestParseEncodeFailureCase(t *testing.T) {
	gidl := `
	encode_failure("OneStringOfMaxLengthFive-too-long") {
		value = OneStringOfMaxLengthFive {
			the_string: "bonjour", // 6 characters
		},
		err = STRING_TOO_LONG,
	}`
	all, err := parse(gidl)
	expectedAll := ir.All{
		EncodeFailure: []ir.EncodeFailure{{
			Name:        "OneStringOfMaxLengthFive-too-long",
			WireFormats: []ir.WireFormat{ir.OldWireFormat, ir.V1WireFormat},
			Value: ir.Record{
				Name: "OneStringOfMaxLengthFive",
				Fields: []ir.Field{
					{
						Key: ir.FieldKey{
							Name: "the_string",
						},
						Value: "bonjour",
					},
				},
			},
			Err: "STRING_TOO_LONG",
		}},
	}
	checkMatch(t, all, expectedAll, err)
}
func TestParseDecodeFailureCase(t *testing.T) {
	gidl := `
	decode_failure("OneStringOfMaxLengthFive-wrong-length") {
		type = TypeName,
		bytes = {
			v1 = [
				1, 0, 0, 0, 0, 0, 0, 0, // length
				255, 255, 255, 255, 255, 255, 255, 255, // alloc present
				// one character missing
			],
		},
		err = STRING_TOO_LONG,
	}`
	all, err := parse(gidl)
	expectedAll := ir.All{
		DecodeFailure: []ir.DecodeFailure{{
			Name: "OneStringOfMaxLengthFive-wrong-length",
			Type: "TypeName",
			Encodings: []ir.Encoding{{
				WireFormat: ir.V1WireFormat,
				Bytes: []byte{
					1, 0, 0, 0, 0, 0, 0, 0, // length
					255, 255, 255, 255, 255, 255, 255, 255, // alloc present
				},
			}},
			Err: "STRING_TOO_LONG",
		}},
	}
	checkMatch(t, all, expectedAll, err)
}

func TestParseBenchmarkCase(t *testing.T) {
	gidl := `
	benchmark("OneStringOfMaxLengthFive-empty") {
		value = OneStringOfMaxLengthFive {
			first: "four",
		},
	}`
	all, err := parse(gidl)
	expectedAll := ir.All{
		DecodeBenchmark: []ir.DecodeBenchmark{{
			Name: "OneStringOfMaxLengthFive-empty",
			Value: ir.Record{
				Name: "OneStringOfMaxLengthFive",
				Fields: []ir.Field{
					{
						Key: ir.FieldKey{
							Name: "first",
						},
						Value: "four",
					},
				},
			},
		}},
		EncodeBenchmark: []ir.EncodeBenchmark{{
			Name: "OneStringOfMaxLengthFive-empty",
			Value: ir.Record{
				Name: "OneStringOfMaxLengthFive",
				Fields: []ir.Field{
					{
						Key: ir.FieldKey{
							Name: "first",
						},
						Value: "four",
					},
				},
			},
		}},
	}
	checkMatch(t, all, expectedAll, err)
}

func TestParseEncodeBenchmarkCase(t *testing.T) {
	gidl := `
	encode_benchmark("OneStringOfMaxLengthFive-empty") {
		value = OneStringOfMaxLengthFive {
			first: "four",
		},
	}`
	all, err := parse(gidl)
	expectedAll := ir.All{
		EncodeBenchmark: []ir.EncodeBenchmark{{
			Name: "OneStringOfMaxLengthFive-empty",
			Value: ir.Record{
				Name: "OneStringOfMaxLengthFive",
				Fields: []ir.Field{
					{
						Key: ir.FieldKey{
							Name: "first",
						},
						Value: "four",
					},
				},
			},
		}},
	}
	checkMatch(t, all, expectedAll, err)
}

func TestParseDecodeBenchmarkCase(t *testing.T) {
	gidl := `
	decode_benchmark("OneStringOfMaxLengthFive-empty") {
		value = OneStringOfMaxLengthFive {
			first: "four",
		},
	}`
	all, err := parse(gidl)
	expectedAll := ir.All{
		DecodeBenchmark: []ir.DecodeBenchmark{{
			Name: "OneStringOfMaxLengthFive-empty",
			Value: ir.Record{
				Name: "OneStringOfMaxLengthFive",
				Fields: []ir.Field{
					{
						Key: ir.FieldKey{
							Name: "first",
						},
						Value: "four",
					},
				},
			},
		}},
	}
	checkMatch(t, all, expectedAll, err)
}

func TestParseSucceedsBindingsAllowlistAndDenylist(t *testing.T) {
	gidl := `
	success("OneStringOfMaxLengthFive-empty") {
		value = OneStringOfMaxLengthFive {
			first: "four",
		},
		bytes = {
			v1 = [
				0, 0, 0, 0, 0, 0, 0, 0, // length
				255, 255, 255, 255, 255, 255, 255, 255, // alloc present
			],
		},
		bindings_allowlist = [go, rust],
		bindings_denylist = [dart],
	}`
	p := NewParser("", strings.NewReader(gidl), []string{"go", "rust", "dart"})
	var all ir.All
	err := p.parseSection(&all)
	expectedAll := ir.All{
		EncodeSuccess: []ir.EncodeSuccess{
			{
				Name: "OneStringOfMaxLengthFive-empty",
				Value: ir.Record{
					Name: "OneStringOfMaxLengthFive",
					Fields: []ir.Field{
						{
							Key: ir.FieldKey{
								Name: "first",
							},
							Value: "four",
						},
					},
				},
				Encodings: []ir.Encoding{{
					WireFormat: ir.V1WireFormat,
					Bytes: []byte{
						0, 0, 0, 0, 0, 0, 0, 0, // length
						255, 255, 255, 255, 255, 255, 255, 255, // alloc present
					},
				}},
				BindingsAllowlist: &ir.LanguageList{"go", "rust"},
				BindingsDenylist:  &ir.LanguageList{"dart"},
			},
		},
		DecodeSuccess: []ir.DecodeSuccess{
			{
				Name: "OneStringOfMaxLengthFive-empty",
				Value: ir.Record{
					Name: "OneStringOfMaxLengthFive",
					Fields: []ir.Field{
						{
							Key: ir.FieldKey{
								Name: "first",
							},
							Value: "four",
						},
					},
				},
				Encodings: []ir.Encoding{{
					WireFormat: ir.V1WireFormat,
					Bytes: []byte{
						0, 0, 0, 0, 0, 0, 0, 0, // length
						255, 255, 255, 255, 255, 255, 255, 255, // alloc present
					},
				}},
				BindingsAllowlist: &ir.LanguageList{"go", "rust"},
				BindingsDenylist:  &ir.LanguageList{"dart"},
			},
		},
	}
	checkMatch(t, all, expectedAll, err)
}

func TestParseFailsBindingsAllowlist(t *testing.T) {
	gidl := `
	success("OneStringOfMaxLengthFive-empty") {
		value = OneStringOfMaxLengthFive {
			first: "four",
		},
		bytes = {
			v1 = [
				0, 0, 0, 0, 0, 0, 0, 0, // length
				255, 255, 255, 255, 255, 255, 255, 255, // alloc present
			],
		},
		bindings_allowlist = [go, rust],
		bindings_denylist = [dart],
	}`
	p := NewParser("", strings.NewReader(gidl), []string{"rust", "dart"})
	var all ir.All
	err := p.parseSection(&all)
	checkFailure(t, err, "invalid language 'go'")
}

func TestParseFailsBindingsDenylist(t *testing.T) {
	gidl := `
	success("OneStringOfMaxLengthFive-empty") {
		value = OneStringOfMaxLengthFive {
			first: "four",
		},
		bytes = {
			v1 = [
				0, 0, 0, 0, 0, 0, 0, 0, // length
				255, 255, 255, 255, 255, 255, 255, 255, // alloc present
			],
		},
		bindings_allowlist = [go, rust],
		bindings_denylist = [dart],
	}`
	p := NewParser("", strings.NewReader(gidl), []string{"rust", "go"})
	var all ir.All
	err := p.parseSection(&all)
	checkFailure(t, err, "invalid language 'dart'")
}

func TestParseSucceedsMultipleWireFormats(t *testing.T) {
	gidl := `
	success("MultipleWireFormats") {
		value = MultipleWireFormats {},
		bytes = {
			old = [0],
			v1 = [1],
		}
	}`
	all, err := parse(gidl)
	expectedAll := ir.All{
		EncodeSuccess: []ir.EncodeSuccess{{
			Name: "MultipleWireFormats",
			Value: ir.Record{
				Name:   "MultipleWireFormats",
				Fields: []ir.Field(nil),
			},
			Encodings: []ir.Encoding{
				{
					WireFormat: ir.OldWireFormat,
					Bytes:      []byte{0},
				},
				{
					WireFormat: ir.V1WireFormat,
					Bytes:      []byte{1},
				},
			},
		}},
		DecodeSuccess: []ir.DecodeSuccess{{
			Name: "MultipleWireFormats",
			Value: ir.Record{
				Name:   "MultipleWireFormats",
				Fields: []ir.Field(nil),
			},
			Encodings: []ir.Encoding{
				{
					WireFormat: ir.OldWireFormat,
					Bytes:      []byte{0},
				},
				{
					WireFormat: ir.V1WireFormat,
					Bytes:      []byte{1},
				},
			},
		}},
	}
	checkMatch(t, all, expectedAll, err)
}

func TestParseFailsExtraKind(t *testing.T) {
	gidl := `
	success("OneStringOfMaxLengthFive-empty") {
		type = Type,
		value = OneStringOfMaxLengthFive {
			first: "four",
		},
		bytes = {
			v1 = [
				0, 0, 0, 0, 0, 0, 0, 0, // length
				255, 255, 255, 255, 255, 255, 255, 255, // alloc present
			],
		},
	}`
	_, err := parse(gidl)
	checkFailure(t, err, "'type' does not apply")
}
func TestParseFailsMissingKind(t *testing.T) {
	gidl := `
	success("OneStringOfMaxLengthFive-empty") {
		value = OneStringOfMaxLengthFive {
			first: "four",
		},
	}`
	_, err := parse(gidl)
	checkFailure(t, err, "missing required parameter 'bytes'")
}
func TestParseFailsUnknownErrorCode(t *testing.T) {
	input := `
	encode_failure("OneStringOfMaxLengthFive-too-long") {
		value = OneStringOfMaxLengthFive {
			the_string: "bonjour",
		},
		err = UNKNOWN_ERROR_CODE,
	}`
	p := NewParser("", strings.NewReader(input), []string{})
	var all ir.All
	if err := p.parseSection(&all); err == nil || !strings.Contains(err.Error(), "unknown error code") {
		t.Errorf("expected 'unknown error code' error, but got %v", err)
	}
}

func TestParseFailsNoBytes(t *testing.T) {
	input := `
	success("NoBytes") {
		value = NoBytes {},
		bytes = {},
	}`
	p := NewParser("", strings.NewReader(input), []string{})
	var all ir.All
	if err := p.parseSection(&all); err == nil || !strings.Contains(err.Error(), "no bytes") {
		t.Errorf("expected 'no bytes' error, but got %v", err)
	}
}

func TestParseFailsDuplicateWireFormat(t *testing.T) {
	input := `
	success("DuplicateWireFormat") {
		value = DuplicateWireFormat {},
		bytes = {
			old = [],
			old = [],
		}
	}`
	p := NewParser("", strings.NewReader(input), []string{})
	var all ir.All
	if err := p.parseSection(&all); err == nil || !strings.Contains(err.Error(), "duplicate wire format") {
		t.Errorf("expected 'duplicate wire format' error, but got %v", err)
	}
}

func parse(gidlInput string) (ir.All, error) {
	p := NewParser("", strings.NewReader(gidlInput), []string{})
	var all ir.All
	err := p.parseSection(&all)
	return all, err
}

func checkMatch(t *testing.T, actual, expected interface{}, err error) {
	if err != nil {
		t.Fatal(err)
	}
	t.Logf("expected: %T %v", expected, expected)
	t.Logf("actual: %T %v", actual, actual)
	if diff := cmp.Diff(expected, actual); diff != "" {
		t.Errorf("expected != actual (-want +got)\n%s", diff)
	}
}

func checkFailure(t *testing.T, err error, errorSubstr string) {
	if err == nil {
		t.Errorf("expected error: %s", errorSubstr)
		return
	}
	if !strings.Contains(err.Error(), errorSubstr) {
		t.Errorf("expected error containing %s, instead got %s", errorSubstr, err.Error())
	}
}

func TestTokenizationSuccess(t *testing.T) {
	cases := map[string][]token{
		"1,2,3": {
			{tText, "1", 1, 1},
			{tComma, ",", 1, 2},
			{tText, "2", 1, 3},
			{tComma, ",", 1, 4},
			{tText, "3", 1, 5},
			{tEof, "", 0, 0},
		},
		"'1', '22'": {
			{tText, "'1'", 1, 1},
			{tComma, ",", 1, 4},
			{tText, "'22'", 1, 6},
		},
	}
	for input, expecteds := range cases {
		t.Run(input, func(t *testing.T) {
			p := NewParser("", strings.NewReader(input), []string{})
			for index, expected := range expecteds {
				actual, err := p.nextToken()
				if err != nil {
					t.Fatalf("unexpected error reading next token: %s", err)
				}
				if actual != expected {
					t.Fatalf(
						"#%d: expected %s (line: %d col: %d), actual %s (line: %d col: %d)", index,
						expected, expected.line, expected.column,
						actual, actual.line, actual.column)
				}
				t.Logf("#%d: %s", index, expected)
			}
		})
	}
}
func TestVariousStringFuncs(t *testing.T) {
	cases := map[fmt.Stringer]string{
		tComma:                          ",",
		tEof:                            "<eof>",
		token{tComma, "whatever", 0, 0}: ",",
		token{tText, "me me me", 0, 0}:  "me me me",
		isValue:                         "value",
	}
	for value, expected := range cases {
		actual := value.String()
		if expected != actual {
			t.Errorf("%v: expected %s, actual %s", value, expected, actual)
		}
	}
}
