///bin/true ; exec /usr/bin/env go run "$0" "$@"
// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package main

import (
	"bufio"
	"flag"
	"fmt"
	"io/ioutil"
	"log"
	"os"
	"os/exec"
	"path"
	"strings"
)

var dryRun = flag.Bool("n", false, "Dry run - print what would happen but don't actually do it")
var symbolDir = flag.String("symbol-dir", "", "Location for the symbol files")
var upload = flag.Bool("upload", true, "Whether to upload the dumped symbols")
var url = flag.String("url", "https://clients2.google.com/cr/symbol", "Endpoint to use")
var verbose = flag.Bool("v", false, "Verbose output")

func getBinariesFromIds(idsFilename string) []string {
	var binaries []string
	file, err := os.Open(idsFilename)
	if err != nil {
		log.Fatal(err)
	}
	defer file.Close()

	scanner := bufio.NewScanner(file)
	for scanner.Scan() {
		binary := strings.SplitAfterN(scanner.Text(), " ", 2)[1]
		binaries = append(binaries, binary)
	}

	if err := scanner.Err(); err != nil {
		log.Fatal(err)
	}

	return binaries
}

func dump(bin string) string {
	if *verbose || *dryRun {
		fmt.Println("Dumping binary", bin)
	}
	symfile := path.Join(*symbolDir, path.Base(bin)+".sym")
	if *dryRun {
		return symfile
	}
	out, err := exec.Command("buildtools/linux-x64/dump_syms/dump_syms", bin).Output()
	if err != nil {
		log.Fatal("could not dump_syms ", bin, ": ", err)
	}
	err = ioutil.WriteFile(symfile, out, 0644)
	if err != nil {
		log.Fatal("could not write output file", symfile, ": ", err)
	}

	return symfile
}

func uploadSymbols(symfile string) {
	if *verbose || *dryRun {
		fmt.Println("Uploading symbols", symfile)
	}
	if *dryRun {
		return
	}

	out, err := exec.Command("buildtools/linux-x64/symupload/sym_upload", symfile, *url).CombinedOutput()
	if err != nil {
		log.Fatal("sym_upload failed with output ", string(out), " error ", err)
	}
}

func mkdir(d string) {
	if *verbose || *dryRun {
		fmt.Println("Making directory", d)
	}
	if *dryRun {
		return
	}
	_, err := exec.Command("mkdir", "-p", d).Output()
	if err != nil {
		log.Fatal("could not create directory", d)
	}
}

func main() {
	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, `Usage ./upload-symbols.go [flags] /path/to/fuchsia/root

This script converts the symbols for a built tree into a format suitable for the
crash server and then optionally uploads them.
`)
		flag.PrintDefaults()
	}

	flag.Parse()

	fuchsiaRoot := flag.Arg(0)
	if _, err := os.Stat(fuchsiaRoot); os.IsNotExist(err) {
		flag.Usage()
		log.Fatalf("Fuchsia root not found at \"%v\"\n", fuchsiaRoot)
	}

	if *symbolDir == "" {
		var err error
		*symbolDir, err = ioutil.TempDir("", "crash-symbols")
		if err != nil {
			log.Fatal("Could not create temporary directory: ", err)
		}
		defer os.RemoveAll(*symbolDir)
	} else if _, err := os.Stat(*symbolDir); os.IsNotExist(err) {
		mkdir(*symbolDir)
	}

	x64BuildDir := "out/release-x64"
	armBuildDir := "out/release-arm64"
	zxBuildDir := "out/build-zircon"
	x64ZxBuildDir := path.Join(zxBuildDir, "build-x64")
	armZxBuildDir := path.Join(zxBuildDir, "build-arm64")
	binaries := getBinariesFromIds(path.Join(fuchsiaRoot, x64BuildDir, "ids.txt"))
	binaries = append(binaries, getBinariesFromIds(path.Join(fuchsiaRoot, armBuildDir, "ids.txt"))...)
	binaries = append(binaries, getBinariesFromIds(path.Join(fuchsiaRoot, x64ZxBuildDir, "ids.txt"))...)
	binaries = append(binaries, getBinariesFromIds(path.Join(fuchsiaRoot, armZxBuildDir, "ids.txt"))...)

	sem := make(chan bool, len(binaries))
	for _, binary := range binaries {
		go func(binary string) {
			symfile := dump(binary)
			if *upload {
				uploadSymbols(symfile)
			}
			sem <- true
		}(binary)
	}
	for i := 0; i < len(binaries); i++ {
		<-sem
	}
}
