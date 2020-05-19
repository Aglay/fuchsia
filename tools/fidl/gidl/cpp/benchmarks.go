// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package cpp

import (
	"bytes"
	fidlir "fidl/compiler/backend/types"
	"fmt"
	gidlir "gidl/ir"
	gidlmixer "gidl/mixer"
	"strings"
	"text/template"
)

var benchmarkTmpl = template.Must(template.New("tmpl").Parse(`
#include <benchmarkfidl/cpp/fidl.h>
#include <perftest/perftest.h>

#include "src/tests/benchmarks/fidl/hlcpp/builder_benchmark_util.h"
#include "src/tests/benchmarks/fidl/hlcpp/decode_benchmark_util.h"
#include "src/tests/benchmarks/fidl/hlcpp/encode_benchmark_util.h"

namespace {

{{ .Type }} Build{{ .Name }}() {
  {{ .ValueBuild }}
  auto result = {{ .ValueVar }};
  return result;
}
bool BenchmarkBuilder{{ .Name }}(perftest::RepeatState* state) {
  return hlcpp_benchmarks::BuilderBenchmark(state, Build{{ .Name }});
}
bool BenchmarkEncode{{ .Name }}(perftest::RepeatState* state) {
  return hlcpp_benchmarks::EncodeBenchmark(state, Build{{ .Name }});
}
bool BenchmarkDecode{{ .Name }}(perftest::RepeatState* state) {
  return hlcpp_benchmarks::DecodeBenchmark(state, Build{{ .Name }});
}

void RegisterTests() {
  perftest::RegisterTest("HLCPP/Builder/{{ .Path }}/WallTime", BenchmarkBuilder{{ .Name }});
  perftest::RegisterTest("HLCPP/Encode/{{ .Path }}/Steps", BenchmarkEncode{{ .Name }});
  perftest::RegisterTest("HLCPP/Decode/{{ .Path }}/Steps", BenchmarkDecode{{ .Name }});
}
PERFTEST_CTOR(RegisterTests)

}  // namespace
`))

type benchmarkTmplInput struct {
	Path, Name, Type     string
	ValueBuild, ValueVar string
}

// Generate generates High-Level C++ benchmarks.
func GenerateBenchmarks(gidl gidlir.All, fidl fidlir.Root) (map[string][]byte, error) {
	schema := gidlmixer.BuildSchema(fidl)
	files := map[string][]byte{}
	for _, gidlBenchmark := range gidl.Benchmark {
		decl, err := schema.ExtractDeclaration(gidlBenchmark.Value)
		if err != nil {
			return nil, fmt.Errorf("benchmark %s: %s", gidlBenchmark.Name, err)
		}
		if gidlir.ContainsUnknownField(gidlBenchmark.Value) {
			continue
		}
		valueBuilder := newCppValueBuilder()
		valueVar := valueBuilder.visit(gidlBenchmark.Value, decl)
		valueBuild := valueBuilder.String()
		var buf bytes.Buffer
		if err := benchmarkTmpl.Execute(&buf, benchmarkTmplInput{
			Path:       gidlBenchmark.Name,
			Name:       benchmarkName(gidlBenchmark.Name),
			Type:       benchmarkTypeFromValue(gidlBenchmark.Value),
			ValueBuild: valueBuild,
			ValueVar:   valueVar,
		}); err != nil {
			return nil, err
		}
		files[benchmarkName("_"+gidlBenchmark.Name)] = buf.Bytes()
	}
	return files, nil
}

func benchmarkTypeFromValue(value gidlir.Value) string {
	return fmt.Sprintf("benchmarkfidl::%s", gidlir.TypeFromValue(value))
}

func benchmarkName(gidlName string) string {
	return strings.ReplaceAll(gidlName, "/", "_")
}
