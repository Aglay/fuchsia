// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package main

import (
	// TODO(kjharland): change crypto/sha1 to a safer hash algorithm. sha256 or sha2, etc.
	"archive/tar"
	"bytes"
	"compress/gzip"
	"context"
	"crypto/sha1"
	"encoding/hex"
	"flag"
	"fmt"
	"io"
	"log"
	"os"
	"os/exec"
	"path"
	"path/filepath"
	"strings"

	"fuchsia.googlesource.com/tools/breakpad"
	"fuchsia.googlesource.com/tools/elflib"
	"fuchsia.googlesource.com/tools/tarutil"
)

const usage = `usage: dump_breakpad_symbols [options] file1 file2 ... fileN

Dumps symbol data from a collection of IDs files. IDs files are generated as
part of the build and contain a number of newline-separate records which have
the syntax:

  <hash-value> <absolute-path>

This command does not care about <hash-value>.  <absolute-path> is the path to a
binary generated as part of the Fuchsia build. This command collects every
<absolute-path> from each of file1, file2 ... fileN and dumps symbol data for
the binaries at each of those paths.  Duplicate paths are skipped.

The output is a collection of symbol files, one for each binary, using an
arbitrary naming scheme to ensure that every output file name is unique.

Example invocation:

$ dump_breakpad_symbols \
	-out-dir=/path/to/output/ \
	-dump-syms-path=/path/to/breakpad/dump_syms \
	-summary-file=/path/to/summary \
	/path/to/ids1.txt
`

// The default module name for modules that don't have a soname, e.g., executables and
// loadable modules. This allows us to use the same module name at runtime as sonames are
// the only names that are guaranteed to be available at build and run times. This value
// must be kept in sync with what Crashpad uses at run time for symbol resolution to work
// properly.
const defaultModuleName = "<_>"

// Command line flag values
var (
	depFilepath  string
	dumpSymsPath string
	outdir       string
	tarFilepath  string
)

func init() {
	flag.Usage = func() {
		fmt.Fprint(os.Stderr, usage)
		flag.PrintDefaults()
		os.Exit(0)
	}

	flag.StringVar(&outdir, "out-dir", "", "The directory where symbol output should be written")
	flag.StringVar(&dumpSymsPath, "dump-syms-path", "", "Path to the breakpad tools `dump_syms` executable")
	flag.StringVar(&depFilepath, "depfile", "", "Path to the ninja depfile to generate")
	flag.StringVar(&tarFilepath, "tar-file", "", "Path where the tar archive containing symbol files is written")
}

func main() {
	flag.Parse()
	if err := execute(context.Background()); err != nil {
		log.Fatal(err)
	}
}

func execute(ctx context.Context) error {
	// Open the input files for reading.  In practice there are very few files,
	// so it's fine to open them all at once.
	var inputReaders []io.Reader
	inputPaths := flag.Args()
	for _, path := range inputPaths {
		file, err := os.Open(path)
		if err != nil {
			return fmt.Errorf("failed to open %s: %v\n", path, err)
		}
		defer file.Close()
		inputReaders = append(inputReaders, file)
	}

	// Process the IDsFiles.
	summary := processIdsFiles(inputReaders, outdir)

	// Write the Ninja dep file.
	depfile := depfile{outputPath: tarFilepath, inputPaths: inputPaths}
	depfd, err := os.Create(depFilepath)
	if err != nil {
		return fmt.Errorf("failed to create file %q: %v", depFilepath, err)
	}
	n, err := depfile.WriteTo(depfd)
	if err != nil {
		return fmt.Errorf("failed to write Ninja dep file %q: %v", depFilepath, err)
	}
	if n == 0 {
		return fmt.Errorf("wrote 0 bytes to %q", depFilepath)
	}

	// Write the tar archive containing all symbol files.
	tarfd, err := os.Create(tarFilepath)
	if err != nil {
		return fmt.Errorf("failed to create %q: %v", tarFilepath, err)
	}
	gzw := gzip.NewWriter(tarfd)
	defer gzw.Close()
	tw := tar.NewWriter(gzw)
	for _, fp := range summary {
		fd, err := os.Open(fp)
		if err != nil {
			return err
		}
		defer fd.Close()
		if err := tarutil.TarReader(tw, fd, fp); err != nil {
			return fmt.Errorf("failed to archive %q: %v", fp, err)
		}
	}
	return nil
}

// processIdsFiles dumps symbol data for each executable in a set of ids files.
func processIdsFiles(idsFiles []io.Reader, outdir string) map[string]string {
	// Binary paths we've already seen.  Duplicates are skipped.
	visited := make(map[string]bool)
	binaryToSymbolFile := make(map[string]string)

	// Iterate through the given set of filepaths.
	for _, idsFile := range idsFiles {
		// Extract the paths to each binary from the IDs file.
		binaries, err := elflib.ReadIDsFile(idsFile)
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			continue
		}

		// Generate the symbol file for each binary.
		for _, bin := range binaries {
			binaryPath := bin.Filepath

			// Check whether we've seen this path already. Skip if so.
			if _, ok := visited[binaryPath]; ok {
				continue
			}
			// Record that we've seen this binary path.
			visited[binaryPath] = true

			symbolFilepath, err := generateSymbolFile(binaryPath)
			if err != nil {
				log.Println(err)
				continue
			}

			// Record the mapping in the summary.
			binaryToSymbolFile[binaryPath] = symbolFilepath
		}
	}

	return binaryToSymbolFile
}

func generateSymbolFile(path string) (outputPath string, err error) {
	outputPath = createSymbolFilepath(outdir, path)
	output, err := exec.Command(dumpSymsPath, path).Output()
	if err != nil {
		return "", fmt.Errorf("failed to generate symbol data for %s: %v", path, err)
	}
	symbolFile, err := breakpad.ParseSymbolFile(bytes.NewReader(output))
	if err != nil {
		return "", fmt.Errorf("failed to read dump_syms output: %v", err)
	}
	// Ensure the module name is either the soname (for shared libraries) or the default
	// value (for executables and loadable modules).
	fd, err := os.Open(path)
	if err != nil {
		return "", fmt.Errorf("failed to open %q: %v", path, err)
	}
	defer fd.Close()
	soname, err := elflib.GetSoName(path, fd)
	if err != nil {
		return "", fmt.Errorf("failed to read soname from %q: %v", path, err)
	}
	if soname == "" {
		symbolFile.ModuleSection.ModuleName = defaultModuleName
	} else {
		symbolFile.ModuleSection.ModuleName = soname
	}

	// Ensure the module section specifies this is a Fuchsia binary instead of Linux
	// binary, which is the default for the dump_syms tool.
	symbolFile.ModuleSection.OS = "Fuchsia"
	symbolFd, err := os.Create(outputPath)
	if err != nil {
		return "", fmt.Errorf("failed to create symbol file %s: %v", outputPath, err)
	}
	defer fd.Close()
	if _, err := symbolFile.WriteTo(symbolFd); err != nil {
		return "", fmt.Errorf("failed to write symbol file %s: %v", outputPath, err)
	}
	return outputPath, nil
}

// Writes the given symbol file data to the given writer after massaging the data.
func writeSymbolFile(w io.Writer, symbolData []byte) error {
	// Many Fuchsia binaries are built as "something.elf", but then packaged as
	// just "something". In the ids.txt file, the name still includes the ".elf"
	// extension, which dump_syms emits into the .sym file, and the crash server
	// uses as part of the lookup.  The binary name and this value written to
	// the .sym file must match, so if the first header line ends in ".elf"
	// strip it off.  This line usually looks something like:
	// MODULE Linux x86_64 094B63014248508BA0636AD3AC3E81D10 sysconf.elf
	lines := strings.SplitN(string(symbolData), "\n", 2)
	if len(lines) != 2 {
		return fmt.Errorf("got <2 lines in symbol data")
	}

	// Make sure the first line is not empty.
	lines[0] = strings.TrimSpace(lines[0])
	if lines[0] == "" {
		return fmt.Errorf("unexpected blank first line in symbol data")
	}

	// Strip .elf from header if it exists.
	if strings.HasSuffix(lines[0], ".elf") {
		lines[0] = strings.TrimSuffix(lines[0], ".elf")
		// Join the new lines of the symbol data.
		symbolData = []byte(strings.Join(lines, "\n"))
	}

	// Write the symbol file.
	_, err := w.Write(symbolData)
	return err
}

// Creates the absolute path to the symbol file for the given binary.
//
// The returned path is generated as a subpath of parentDir.
func createSymbolFilepath(parentDir string, binaryPath string) string {
	// Create the symbole file basename as a hash of the path to the binary.
	// This ensures that filenames are unique within the output directory.
	hash := sha1.New()
	n, err := hash.Write([]byte(binaryPath))
	if err != nil {
		panic(err)
	}
	if n == 0 {
		// Empty text should never be passed to this function and likely signifies
		// an error in the input file. Panic here as well.
		panic("0 bytes written for hash of input text '" + binaryPath + "'")
	}
	basename := hex.EncodeToString(hash.Sum(nil)) + ".sym"

	// Generate the filepath as an subdirectory of the given parent directory.
	absPath, err := filepath.Abs(path.Join(parentDir, basename))
	if err != nil {
		// Panic because if this fails once it's likely to keep failing.
		panic(fmt.Sprintf("failed to get path to symbol file for %s: %v", binaryPath, err))
	}

	return absPath
}
