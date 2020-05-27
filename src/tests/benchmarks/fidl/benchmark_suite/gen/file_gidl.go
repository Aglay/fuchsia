// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package main

import (
	"gen/config"
	gidlutil "gen/gidl/util"
	"log"
	"os"
	"path"
	"strings"
	"text/template"
	"time"
)

var gidlTmpl = template.Must(template.New("gidlTmpl").Parse(
	`// Copyright {{ .Year }} The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// GENERATED FILE: Regen with $(fx get-build-dir)/host-tools/regen_fidl_benchmark_suite

{{- range .Benchmarks }}

{{ .Comment -}}
benchmark("{{ .Name }}") {
    {{- if .Allowlist }}
    bindings_allowlist = {{ .Allowlist }},
    {{- end -}}
    {{- if .Denylist }}
    bindings_denylist = {{ .Denylist }},
	{{- end }}
	{{- if .EnableSendEventBenchmark }}
	enable_send_event_benchmark = true,
	{{- end }}
    value = {{ .Value }},
}
{{- end }}
`))

type gidlTmplInput struct {
	Year       int
	Benchmarks []gidlTmplBenchmark
}

type gidlTmplBenchmark struct {
	Name                     string
	Comment                  string
	Value                    string
	Allowlist, Denylist      string
	EnableSendEventBenchmark bool
}

func genGidlFile(filepath string, gidl config.GidlFile) error {
	var results []gidlTmplBenchmark
	for _, benchmark := range gidl.Benchmarks {
		value, err := gidl.Gen(benchmark.Config)
		if err != nil {
			return err
		}
		results = append(results, gidlTmplBenchmark{
			Name:                     benchmark.Name,
			Comment:                  formatComment(benchmark.Comment),
			Value:                    formatObj(1, value),
			Allowlist:                formatBindingList(benchmark.Allowlist),
			Denylist:                 formatBindingList(benchmark.Denylist),
			EnableSendEventBenchmark: benchmark.EnableSendEventBenchmark,
		})
	}

	f, err := os.Create(filepath)
	if err != nil {
		return err
	}
	defer f.Close()
	return gidlTmpl.Execute(f, gidlTmplInput{
		Year:       time.Now().Year(),
		Benchmarks: results,
	})
}

func genGidl(outdir string) {
	for _, gidl := range gidlutil.AllGidlFiles() {
		if !strings.HasSuffix(gidl.Filename, ".gen.gidl") {
			log.Fatalf("%s needs .gen.gidl suffix", gidl.Filename)
		}
		filepath := path.Join(outdir, gidl.Filename)
		if err := genGidlFile(filepath, gidl); err != nil {
			log.Fatalf("Error generating %s: %s", gidl.Filename, err)
		}
	}
}
