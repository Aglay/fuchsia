// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package walker

import (
	"bytes"
	"fmt"
	"strings"
	"text/template"

	fidlir "fidl/compiler/backend/types"
	gidlir "gidl/ir"
	libllcpp "gidl/llcpp/lib"
	gidlmixer "gidl/mixer"
)

var benchmarkTmpl = template.Must(template.New("tmpl").Parse(`
#include <benchmarkfidl/llcpp/fidl.h>
#include <perftest/perftest.h>

#include "src/tests/benchmarks/fidl/walker/walker_benchmark_util.h"

namespace {

void Build{{ .Name }}(std::function<void({{.Type}})> f) {
	{{ .ValueBuild }}
	f(std::move({{ .ValueVar }}));
}

bool BenchmarkWalker{{ .Name }}(perftest::RepeatState* state) {
	return walker_benchmarks::WalkerBenchmark<{{ .Type }}>(state, Build{{ .Name }});
}

void RegisterTests() {
	perftest::RegisterTest("Walker/{{ .Path }}/WallTime", BenchmarkWalker{{ .Name }});
}
PERFTEST_CTOR(RegisterTests)

} // namespace
`))

type benchmarkTmplInput struct {
	Path, Name, Type     string
	ValueBuild, ValueVar string
}

func GenerateBenchmarks(gidl gidlir.All, fidl fidlir.Root) ([]byte, map[string][]byte, error) {
	schema := gidlmixer.BuildSchema(fidl)
	files := map[string][]byte{}
	for _, gidlBenchmark := range gidl.Benchmark {
		decl, err := schema.ExtractDeclaration(gidlBenchmark.Value)
		if err != nil {
			return nil, nil, fmt.Errorf("walker benchmark %s: %s", gidlBenchmark.Name, err)
		}
		if gidlir.ContainsUnknownField(gidlBenchmark.Value) {
			continue
		}
		valBuild, valVar := libllcpp.BuildValueUnowned(gidlBenchmark.Value, decl)
		var buf bytes.Buffer
		if err := benchmarkTmpl.Execute(&buf, benchmarkTmplInput{
			Path:       gidlBenchmark.Name,
			Name:       benchmarkName(gidlBenchmark.Name),
			Type:       llcppBenchmarkType(gidlBenchmark.Value),
			ValueBuild: valBuild,
			ValueVar:   valVar,
		}); err != nil {
			return nil, nil, err
		}
		files[benchmarkName("_"+gidlBenchmark.Name)] = buf.Bytes()
	}
	return nil, files, nil
}

func llcppBenchmarkType(value gidlir.Value) string {
	return fmt.Sprintf("llcpp::benchmarkfidl::%s", gidlir.TypeFromValue(value))
}

func benchmarkName(gidlName string) string {
	return strings.ReplaceAll(gidlName, "/", "_")
}
