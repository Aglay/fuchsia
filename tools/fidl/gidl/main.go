// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"io/ioutil"
	"log"
	"os"
	"path/filepath"
	"sort"
	"strings"

	fidlir "go.fuchsia.dev/fuchsia/garnet/go/src/fidl/compiler/backend/types"
	gidlconfig "go.fuchsia.dev/fuchsia/tools/fidl/gidl/config"
	gidlcpp "go.fuchsia.dev/fuchsia/tools/fidl/gidl/cpp"
	gidldart "go.fuchsia.dev/fuchsia/tools/fidl/gidl/dart"
	gidlgolang "go.fuchsia.dev/fuchsia/tools/fidl/gidl/golang"
	gidlir "go.fuchsia.dev/fuchsia/tools/fidl/gidl/ir"
	gidlllcpp "go.fuchsia.dev/fuchsia/tools/fidl/gidl/llcpp"
	gidlparser "go.fuchsia.dev/fuchsia/tools/fidl/gidl/parser"
	gidlreference "go.fuchsia.dev/fuchsia/tools/fidl/gidl/reference"
	gidlrust "go.fuchsia.dev/fuchsia/tools/fidl/gidl/rust"
	gidlwalker "go.fuchsia.dev/fuchsia/tools/fidl/gidl/walker"
)

// Generator is a function that generates conformance tests for a particular
// backend and returns a map of test name to file bytes. The test name is
// added as a suffix to the name of the file before the extension
// (e.g. my_file.go -> my_file_test_name.go).
// The first file is the "main output file".
type Generator func(gidlir.All, fidlir.Root, gidlconfig.GeneratorConfig) ([]byte, map[string][]byte, error)

var conformanceGenerators = map[string]Generator{
	"go":    gidlgolang.GenerateConformanceTests,
	"llcpp": gidlllcpp.GenerateConformanceTests,
	"cpp":   gidlcpp.GenerateConformanceTests,
	"dart":  gidldart.GenerateConformanceTests,
	"rust":  gidlrust.GenerateConformanceTests,
}

var benchmarkGenerators = map[string]Generator{
	"go":        gidlgolang.GenerateBenchmarks,
	"llcpp":     gidlllcpp.GenerateBenchmarks,
	"cpp":       gidlcpp.GenerateBenchmarks,
	"rust":      gidlrust.GenerateBenchmarks,
	"dart":      gidldart.GenerateBenchmarks,
	"reference": gidlreference.GenerateBenchmarks,
	"walker":    gidlwalker.GenerateBenchmarks,
}

var allGenerators = map[string]map[string]Generator{
	"conformance": conformanceGenerators,
	"benchmark":   benchmarkGenerators,
}

var allGeneratorTypes = func() []string {
	var list []string
	for generatorType := range allGenerators {
		list = append(list, generatorType)
	}
	sort.Strings(list)
	return list
}()

var allLanguages = func() []string {
	var list []string
	seen := make(map[string]struct{})
	for _, generatorMap := range allGenerators {
		for language := range generatorMap {
			if _, ok := seen[language]; !ok {
				seen[language] = struct{}{}
				list = append(list, language)
			}
		}
	}
	sort.Strings(list)
	return list
}()

var allWireFormats = []gidlir.WireFormat{
	gidlir.V1WireFormat,
}

// GIDLFlags stores the command-line flags for the GIDL program.
type GIDLFlags struct {
	JSONPath *string
	Language *string
	Type     *string
	// TODO(fxbug.dev/52371) It should not be necessary to specify the number of generated files.
	NumOutputFiles            *int
	MultipleFilePattern       *string
	Out                       *string
	RustBenchmarksFidlLibrary *string
}

// Valid indicates whether the parsed Flags are valid to be used.
func (gidlFlags GIDLFlags) Valid() bool {
	return len(*gidlFlags.JSONPath) != 0 && flag.NArg() != 0
}

var flags = GIDLFlags{
	JSONPath: flag.String("json", "",
		"relative path to the FIDL intermediate representation."),
	Language: flag.String("language", "",
		fmt.Sprintf("target language (%s)", strings.Join(allLanguages, "/"))),
	Type: flag.String("type", "", fmt.Sprintf("output type (%s)", strings.Join(allGeneratorTypes, "/"))),
	NumOutputFiles: flag.Int("num-extra-output-files", 0,
		`Upper bound on number of extra output files when there are multiple files.
	Used to split C++ outputs to multiple files.
	Must be at least as large as the actual number of outputs.`),
	MultipleFilePattern: flag.String("extra-file-pattern", "",
		`String including a [NUM] substring to be replaced with a number.
This is used for generating output filenames when there are multiple files`),
	Out: flag.String("out", "", "optional path to write output to"),
	RustBenchmarksFidlLibrary: flag.String("rust-benchmarks-fidl-library", "",
		"name for the fidl library used in the rust benchmarks"),
}

func parseGidlIr(filename string) gidlir.All {
	f, err := os.Open(filename)
	if err != nil {
		panic(err)
	}
	config := gidlparser.Config{
		Languages:   allLanguages,
		WireFormats: allWireFormats,
	}
	result, err := gidlparser.NewParser(filename, f, config).Parse()
	if err != nil {
		panic(err)
	}
	return result
}

func parseFidlJSONIr(filename string) fidlir.Root {
	bytes, err := ioutil.ReadFile(filename)
	if err != nil {
		panic(err)
	}
	var result fidlir.Root
	if err := json.Unmarshal(bytes, &result); err != nil {
		panic(err)
	}
	return result
}

func main() {
	flag.Parse()

	if !flag.Parsed() || !flags.Valid() {
		flag.PrintDefaults()
		os.Exit(1)
	}

	var config gidlconfig.GeneratorConfig
	if *flags.RustBenchmarksFidlLibrary != "" {
		config.RustBenchmarksFidlLibrary = *flags.RustBenchmarksFidlLibrary
	}

	fidl := parseFidlJSONIr(*flags.JSONPath)

	var parsedGidlFiles []gidlir.All
	for _, path := range flag.Args() {
		parsedGidlFiles = append(parsedGidlFiles, parseGidlIr(path))
	}
	gidl := gidlir.FilterByBinding(gidlir.Merge(parsedGidlFiles), *flags.Language)

	// For simplicity, we do not allow FIDL that GIDL depends on to have
	// dependent libraries. This makes it much simpler to have everything
	// in the IR, and avoid cross-references.

	// TODO(fxbug.dev/7802): While transitioning "zx" from [Internal] to a normal
	// library, tolerate but ignore a dependency on zx.
	if len(fidl.Libraries) == 1 && fidl.Libraries[0].Name == "zx" {
		fidl.Libraries = make([]fidlir.Library, 0)
	}

	if len(fidl.Libraries) != 0 {
		var libs []string
		for _, l := range fidl.Libraries {
			libs = append(libs, string(l.Name))
		}
		panic(fmt.Sprintf(
			"GIDL does not work with FIDL libraries with dependents, found: %s",
			strings.Join(libs, ",")))
	}

	language := *flags.Language
	if language == "" {
		panic("must specify --language")
	}

	gidlir.ValidateAllType(gidl, *flags.Type)
	generatorMap, ok := allGenerators[*flags.Type]
	if !ok {
		panic(fmt.Sprintf("unknown generator type: %s", *flags.Type))
	}
	generator, ok := generatorMap[language]
	if !ok {
		log.Fatalf("unknown language for %s: %s", *flags.Type, language)
	}

	mainFile, otherFiles, err := generator(gidl, fidl, config)
	if err != nil {
		log.Fatal(err)
	}

	if mainFile != nil {
		if *flags.Out == "" {
			log.Fatalf("no -out path specified for main file")
		}
		if err := ioutil.WriteFile(*flags.Out, mainFile, 0666); err != nil {
			log.Fatal(err)
		}
	}

	if len(otherFiles) > *flags.NumOutputFiles {
		log.Fatalf("more generated gidl outputs than the number of output files")
	}
	if *flags.MultipleFilePattern == "" {
		if *flags.NumOutputFiles != 0 {
			log.Fatalf("no pattern specified for multiple output files")
		}
		return
	}

	os.Remove(filepath.Dir(*flags.MultipleFilePattern)) // clear any existing directory
	if err := os.MkdirAll(filepath.Dir(*flags.MultipleFilePattern), os.ModePerm); err != nil {
		log.Fatal(err)
	}

	// TODO(fxbug.dev/52371) Until these files are named based on their `type`, they need to be
	// generated in a deterministic order.
	fileNum := 1
	keys := make([]string, 0, len(otherFiles))
	for k := range otherFiles {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	for _, k := range keys {
		if err := ioutil.WriteFile(outputFilepath(*flags.MultipleFilePattern, fileNum), otherFiles[k], 0666); err != nil {
			log.Fatal(err)
		}
		fileNum++
	}
	// Fill unused output files with empty files to have a consistent set of source dependencies
	// in GN.
	// TODO(fxbug.dev/52371) The empty file doesn't work for some languages, like go that require a
	// package to be declared.
	for ; fileNum <= *flags.NumOutputFiles; fileNum++ {
		if err := ioutil.WriteFile(outputFilepath(*flags.MultipleFilePattern, fileNum), []byte{}, 0666); err != nil {
			log.Fatal(err)
		}
	}
}

// x/y/abc[NUM].test.go -> x/y/abc123.test.go
func outputFilepath(inputFilepath string, fileNum int) string {
	return strings.ReplaceAll(inputFilepath, "[NUM]", fmt.Sprintf("%d", fileNum))
}
