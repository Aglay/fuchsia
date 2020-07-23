// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"io/ioutil"
	"log"
	"strings"

	"go.fuchsia.dev/fuchsia/tools/build"
	"go.fuchsia.dev/fuchsia/tools/testing/affectedtests"
)

func main() {
	srcs := flag.String("srcs", "", "Source files changed (path relative to root build dir)")
	testsJSON := flag.String("tests_json", "", "Generated tests.json")
	dotFile := flag.String("graph", "", "Ninja graph to analyze. Generate with `ninja -C out/default -t graph`")
	flag.Parse()

	testsJSONContents, err := ioutil.ReadFile(*testsJSON)
	if err != nil {
		log.Fatal("Failed to read tests.json from ", *testsJSON, ": ", err)
	}
	var testSpecs []build.TestSpec
	if err = json.Unmarshal(testsJSONContents, &testSpecs); err != nil {
		log.Fatal("Failed to parse tests.json from ", *testsJSON, ": ", err)
	}

	dotFileContents, err := ioutil.ReadFile(*dotFile)
	if err != nil {
		log.Fatal("Failed to read graph from ", *dotFile, ": ", err)
	}

	affected := affectedtests.AffectedTests(strings.Fields(*srcs), testSpecs, dotFileContents)

	for _, test := range affected {
		fmt.Println(test)
	}
}
