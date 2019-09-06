// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package main

import (
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"os"

	"go.fuchsia.dev/fuchsia/tools/debug/symbolize/lib"
	"go.fuchsia.dev/fuchsia/tools/lib/color"
	"go.fuchsia.dev/fuchsia/tools/lib/logger"
)

type argList []string

func (a *argList) String() string {
	return fmt.Sprintf("%s", []string(*a))
}

func (a *argList) Set(value string) error {
	*a = append(*a, value)
	return nil
}

var (
	buildIDDirPaths argList
	colors          color.EnableColor
	jsonOutput      string
	idsPaths        argList
	// TODO(jakehehrlich): Make idsRel always true and remove this flag.
	idsRel        bool
	level         logger.LogLevel
	llvmSymboPath string
)

func init() {
	colors = color.ColorAuto
	level = logger.InfoLevel

	flag.Var(&buildIDDirPaths, "build-id-dir", "path to .build-id directory")
	flag.StringVar(&llvmSymboPath, "llvm-symbolizer", "llvm-symbolizer", "path to llvm-symbolizer")
	flag.Var(&idsPaths, "ids", "path to ids.txt")
	flag.Var(&colors, "color", "use color in output, can be never, auto, always")
	flag.Var(&level, "level", "output verbosity, can be fatal, error, warning, info, debug or trace")
	flag.StringVar(&jsonOutput, "json-output", "", "outputs trigger information to the specified file")
	flag.BoolVar(&idsRel, "ids-rel", false, "tells the symbolizer to always use ids.txt relative paths")
}

type dumpEntry struct {
	Modules  []symbolize.Module  `json:"modules"`
	Segments []symbolize.Segment `json:"segments"`
	Type     string              `json:"type"`
	Name     string              `json:"name"`
}

type dumpHandler struct {
	dumps []dumpEntry
}

func (d *dumpHandler) HandleDump(dump *symbolize.DumpfileElement) {
	triggerCtx := dump.Context()
	d.dumps = append(d.dumps, dumpEntry{
		Modules:  triggerCtx.Mods,
		Segments: triggerCtx.Segs,
		Type:     dump.SinkType(),
		Name:     dump.Name(),
	})
}

func (d *dumpHandler) Write(buf io.Writer) error {
	enc := json.NewEncoder(buf)
	enc.SetIndent("", "  ")
	err := enc.Encode(d.dumps)
	if err != nil {
		return err
	}
	return nil
}

func main() {
	// Parse flags and setup helpers
	flag.Parse()
	var jsonTriggerHandler *dumpHandler
	if jsonOutput != "" {
		jsonTriggerHandler = &dumpHandler{}
	}

	// Setup logger and context
	painter := color.NewColor(colors)
	log := logger.NewLogger(level, painter, os.Stdout, os.Stderr, "")
	ctx := logger.WithLogger(context.Background(), log)

	// Construct the nodes of the pipeline
	symbolizer := symbolize.NewLLVMSymbolizer(llvmSymboPath)
	var repo symbolize.CompositeRepo
	for _, dir := range buildIDDirPaths {
		repo.AddRepo(symbolize.NewBuildIDRepo(dir))
	}
	for _, idsPath := range idsPaths {
		repo.AddRepo(symbolize.NewIDsTxtRepo(idsPath, idsRel))
	}
	demuxer := symbolize.NewDemuxer(&repo, symbolizer)
	tap := symbolize.NewTriggerTap()
	if jsonTriggerHandler != nil {
		tap.AddHandler(jsonTriggerHandler.HandleDump)
	}
	presenter := symbolize.NewBasicPresenter(os.Stdout, painter.Enabled())

	// Build the pipeline to start presenting.
	if err := symbolizer.Start(ctx); err != nil {
		log.Fatalf("%v\n", err)
	}
	inputLines := symbolize.StartParsing(ctx, os.Stdin)
	outputLines := demuxer.Start(ctx, inputLines)
	trash := symbolize.ComposePostProcessors(ctx, outputLines,
		tap,
		&symbolize.ContextPresenter{},
		&symbolize.OptimizeColor{},
		symbolize.NewBacktracePresenter(os.Stdout, presenter))
	symbolize.Consume(trash)

	// Once the pipeline has finished output all triggers
	if jsonTriggerHandler != nil {
		file, err := os.Create(jsonOutput)
		if err != nil {
			log.Fatalf("%v\n", err)
		}
		if err := jsonTriggerHandler.Write(file); err != nil {
			log.Fatalf("%v\n", err)
		}
	}
}
