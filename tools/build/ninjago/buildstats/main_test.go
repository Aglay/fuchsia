// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.package main

package main

import (
	"bytes"
	"compress/gzip"
	"encoding/json"
	"errors"
	"flag"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/google/go-cmp/cmp"
	"github.com/google/go-cmp/cmp/cmpopts"
	"go.fuchsia.dev/fuchsia/tools/build/ninjago/compdb"
	"go.fuchsia.dev/fuchsia/tools/build/ninjago/ninjalog"
)

var testDataDir = flag.String("test_data_dir", "../test_data", "Path to ../test_data/; only used in GN build")

func readAndUnzip(t *testing.T, path string) *gzip.Reader {
	f, err := os.Open(path)
	if err != nil {
		t.Fatalf("Failed to read %q: %v", path, err)
	}
	t.Cleanup(func() { f.Close() })

	unzipped, err := gzip.NewReader(f)
	if err != nil {
		t.Fatalf("Failed to unzip %q: %v", path, err)
	}
	t.Cleanup(func() { unzipped.Close() })
	return unzipped
}

func TestExtractAndSerializeBuildStats(t *testing.T) {
	graph, steps, err := constructGraph(inputs{
		ninjalog: readAndUnzip(t, filepath.Join(*testDataDir, "ninja_log.gz")),
		compdb:   readAndUnzip(t, filepath.Join(*testDataDir, "compdb.json.gz")),
		graph:    readAndUnzip(t, filepath.Join(*testDataDir, "graph.dot.gz")),
	})
	if err != nil {
		t.Fatalf("Failed to construct graph: %v", err)
	}

	stats, err := extractBuildStats(&graph, steps)
	if err != nil {
		t.Fatalf("Failed to extract build stats: %v", err)
	}
	if len(stats.CriticalPath) == 0 {
		t.Errorf("Critical path in stats is emtpy, expect non-empty")
	}
	if len(stats.Slowests) == 0 {
		t.Errorf("Slowest builds in stats is empty, expect non-empty")
	}
	if len(stats.CatBuildTimes) == 0 {
		t.Errorf("Build times by category in stats is empty, expect non-empty")
	}

	buffer := new(bytes.Buffer)
	if err := serializeBuildStats(stats, buffer); err != nil {
		t.Fatalf("Failed to serialize build stats: %v", err)
	}
	var gotStats buildStats
	if err := json.NewDecoder(buffer).Decode(&gotStats); err != nil {
		t.Fatalf("Failed to deserialize build stats: %v", err)
	}
	if diff := cmp.Diff(stats, gotStats); diff != "" {
		t.Errorf("build stats diff after deserialization (-want, +got):\n%s", diff)
	}
}

type stubGraph struct {
	criticalPath []ninjalog.Step
	err          error
}

func (g *stubGraph) CriticalPath() ([]ninjalog.Step, error) {
	return g.criticalPath, g.err
}

func TestExtractStats(t *testing.T) {
	for _, v := range []struct {
		name  string
		g     stubGraph
		steps []ninjalog.Step
		want  buildStats
	}{
		{
			name: "empty steps",
		},
		{
			name: "successfully extract stats",
			g: stubGraph{
				criticalPath: []ninjalog.Step{
					{
						CmdHash: 1,
						Out:     "a.o",
						Outs:    []string{"aa.o", "aaa.o"},
						End:     3 * time.Second,
						Command: &compdb.Command{Command: "gomacc a.cc"},
					},
					{
						CmdHash: 2,
						Out:     "b.o",
						Start:   3 * time.Second,
						End:     5 * time.Second,
						Command: &compdb.Command{Command: "rustc b.rs"},
					},
				},
			},
			steps: []ninjalog.Step{
				{
					CmdHash: 1,
					Out:     "a.o",
					Outs:    []string{"aa.o", "aaa.o"},
					End:     3 * time.Second,
					Command: &compdb.Command{Command: "gomacc a.cc"},
				},
				{
					CmdHash: 2,
					Out:     "b.o",
					Start:   3 * time.Second,
					End:     5 * time.Second,
					Command: &compdb.Command{Command: "rustc b.rs"},
				},
				{
					CmdHash: 3,
					Out:     "c.o",
					Start:   9 * time.Second,
					End:     10 * time.Second,
					Command: &compdb.Command{Command: "gomacc c.cc"},
				},
			},
			want: buildStats{
				CriticalPath: []action{
					{
						Command:  "gomacc a.cc",
						Outputs:  []string{"aa.o", "aaa.o", "a.o"},
						End:      3 * time.Second,
						Category: "gomacc",
					},
					{
						Command:  "rustc b.rs",
						Outputs:  []string{"b.o"},
						Start:    3 * time.Second,
						End:      5 * time.Second,
						Category: "rustc",
					},
				},
				Slowests: []action{
					{
						Command:  "gomacc a.cc",
						Outputs:  []string{"aa.o", "aaa.o", "a.o"},
						End:      3 * time.Second,
						Category: "gomacc",
					},
					{
						Command:  "rustc b.rs",
						Outputs:  []string{"b.o"},
						Start:    3 * time.Second,
						End:      5 * time.Second,
						Category: "rustc",
					},
					{
						Command:  "gomacc c.cc",
						Outputs:  []string{"c.o"},
						Start:    9 * time.Second,
						End:      10 * time.Second,
						Category: "gomacc",
					},
				},
				CatBuildTimes: []catBuildTime{
					{
						Category:     "gomacc",
						Count:        2,
						BuildTime:    4 * time.Second,
						MinBuildTime: time.Second,
						MaxBuildTime: 3 * time.Second,
					},
					{
						Category:     "rustc",
						Count:        1,
						BuildTime:    2 * time.Second,
						MinBuildTime: 2 * time.Second,
						MaxBuildTime: 2 * time.Second,
					},
				},
				TotalBuildTime: 6 * time.Second,
				BuildDuration:  10 * time.Second,
			},
		},
	} {
		t.Run(v.name, func(t *testing.T) {
			gotStats, err := extractBuildStats(&v.g, v.steps)
			if err != nil {
				t.Fatalf("extractBuildStats(%#v, %#v) got error: %v", v.g, v.steps, err)
			}
			if diff := cmp.Diff(v.want, gotStats, cmpopts.EquateEmpty()); diff != "" {
				t.Errorf("extractBuildStats(%#v, %#v) got stats diff (-want +got):\n%s", v.g, v.steps, diff)
			}
		})
	}
}

func TestExtractStatsError(t *testing.T) {
	g := stubGraph{err: errors.New("test critical path error")}
	if _, err := extractBuildStats(&g, nil); err == nil {
		t.Errorf("extractBuildStats(%#v, nil) got no error, want error", g)
	}
}
