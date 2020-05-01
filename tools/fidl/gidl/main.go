// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package main

import (
	"bytes"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"io/ioutil"
	"os"
	"path/filepath"
	"sort"
	"strings"

	fidlir "fidl/compiler/backend/types"
	gidlcpp "gidl/cpp"
	gidldart "gidl/dart"
	gidlgolang "gidl/golang"
	gidlir "gidl/ir"
	gidlllcpp "gidl/llcpp"
	gidlparser "gidl/parser"
	gidlrust "gidl/rust"
	gidltransformer "gidl/transformer"
)

// Generator is a function that generates conformance tests for a particular
// backend and writes them to the io.Writer.
type Generator func(io.Writer, gidlir.All, fidlir.Root) error

var conformanceGenerators = map[string]Generator{
	"go":          gidlgolang.GenerateConformanceTests,
	"llcpp":       gidlllcpp.GenerateConformanceTests,
	"cpp":         gidlcpp.GenerateConformanceTests,
	"dart":        gidldart.Generate,
	"rust":        gidlrust.GenerateConformanceTests,
	"transformer": gidltransformer.Generate,
}

var benchmarkGenerators = map[string]Generator{
	"go":    gidlgolang.GenerateBenchmarks,
	"llcpp": gidlllcpp.GenerateBenchmarks,
	"cpp":   gidlcpp.GenerateBenchmarks,
	"rust":  gidlrust.GenerateBenchmarks,
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
	for _, generatorMap := range allGenerators {
		for language := range generatorMap {
			list = append(list, language)
		}
	}
	sort.Strings(list)
	return list
}()

// GIDLFlags stores the command-line flags for the GIDL program.
type GIDLFlags struct {
	JSONPath *string
	Language *string
	Type     *string
	Out      *string
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
	Out:  flag.String("out", "-", "optional path to write output to"),
}

func parseGidlIr(filename string) gidlir.All {
	f, err := os.Open(filename)
	if err != nil {
		panic(err)
	}
	result, err := gidlparser.NewParser(filename, f, allLanguages).Parse()
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
	buf := new(bytes.Buffer)

	gidlir.ValidateAllType(gidl, *flags.Type)
	generatorMap, ok := allGenerators[*flags.Type]
	if !ok {
		panic(fmt.Sprintf("unknown generator type: %s", *flags.Type))
	}
	generator, ok := generatorMap[language]
	if !ok {
		panic(fmt.Sprintf("unknown language: %s", language))
	}

	err := generator(buf, gidl, fidl)
	if err != nil {
		panic(err)
	}
	var writer = os.Stdout
	if *flags.Out != "-" {
		err := os.MkdirAll(filepath.Dir(*flags.Out), os.ModePerm)
		if err != nil {
			panic(err)
		}
		writer, err = os.Create(*flags.Out)
		if err != nil {
			panic(err)
		}
		defer writer.Close()
	}

	_, err = writer.Write(buf.Bytes())
	if err != nil {
		panic(err)
	}
}
