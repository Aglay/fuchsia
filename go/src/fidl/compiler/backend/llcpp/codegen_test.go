// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
package llcpp

import (
	"bytes"
	"fmt"
	"strings"
	"testing"

	"fidl/compiler/backend/cpp/ir"
	"fidl/compiler/backend/typestest"
)

type example string

func (s example) header() string {
	return fmt.Sprintf("%s.llcpp.h.golden", s)
}

func (s example) source() string {
	return fmt.Sprintf("%s.llcpp.cpp.golden", s)
}

var cases = []example{
	"empty_struct.fidl.json",
	"union.fidl.json",
}

func TestCodegenHeader(t *testing.T) {
	for _, filename := range cases {
		t.Run(string(filename), func(t *testing.T) {
			fidl := typestest.GetExample(string(filename))
			tree := ir.Compile(fidl)
			tree.PrimaryHeader = strings.TrimRight(filename.header(), ".golden")
			header := typestest.GetGolden(filename.header())

			buf := new(bytes.Buffer)
			if err := NewFidlGenerator().GenerateHeader(buf, tree); err != nil {
				t.Fatalf("unexpected error while generating header: %s", err)
			}

			typestest.AssertCodegenCmp(t, header, buf.Bytes())
		})
	}
}
func TestCodegenSource(t *testing.T) {
	for _, filename := range cases {
		t.Run(string(filename), func(t *testing.T) {
			fidl := typestest.GetExample(string(filename))
			tree := ir.Compile(fidl)
			tree.PrimaryHeader = strings.TrimRight(filename.header(), ".golden")
			source := typestest.GetGolden(filename.source())

			buf := new(bytes.Buffer)
			if err := NewFidlGenerator().GenerateSource(buf, tree); err != nil {
				t.Fatalf("unexpected error while generating source: %s", err)
			}

			typestest.AssertCodegenCmp(t, source, buf.Bytes())
		})
	}
}
