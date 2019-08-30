// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
package main

import (
	"bufio"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"io/ioutil"
	"log"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

type Input struct {
	AssetLimit json.Number `json:"asset_limit"`
	CoreLimit  json.Number `json:"core_limit"`
	AssetExt   []string    `json:"asset_ext"`
	Components []Component `json:"components"`
}

type Node struct {
	fullPath string
	size     int64
	children map[string]*Node
}

type Component struct {
	Component string      `json:"component"`
	Limit     json.Number `json:"limit"`
	Src       []string    `json:"src"`
}

type Blob struct {
	dep  []string
	size int64
	name string
	hash string
}

type BlobFromJSON struct {
	Merkle     string `json:"merkle"`
	Path       string `json:"path"`
	SourcePath string `json:"source_path"`
}

const (
	MetaFar   = "meta.far"
	BlobSizes = "blob.sizes"
	BlobList  = "gen/build/images/blob.manifest.list"
	BlobsJSON = "blobs.json"
)

func newDummyNode() *Node {
	return newNode("")
}

func newNode(p string) *Node {
	return &Node{
		fullPath: p,
		children: make(map[string]*Node),
	}
}

// Breaks down the given path divided by "/" and updates the size of each node on the path with the
// size of the given blob.
func (root *Node) add(p string, blob *Blob) {
	fullPath := strings.Split(p, "/")
	curr := root
	var nodePath string
	// A blob may be used by many packages, so the size of each blob is the total space allocated to
	// that blob in blobfs.
	// We divide the size by the total number of packages that depend on it to get a rough estimate of
	// the size of the individual blob.
	size := blob.size / int64(len(blob.dep))
	for _, name := range fullPath {
		nodePath = filepath.Join(nodePath, name)
		if _, ok := curr.children[name]; !ok {
			target := newNode(nodePath)
			curr.children[name] = target
		}
		curr = curr.children[name]
		curr.size += size
	}
}

// Finds the node whose fullPath matches the given path. The path is guaranteed to be unique as it
// corresponds to the filesystem of the build directory.
func (root *Node) find(p string) *Node {
	fullPath := strings.Split(p, "/")
	curr := root

	for _, name := range fullPath {
		next := curr.children[name]
		if next == nil {
			return nil
		}
		curr = next
	}

	return curr
}

// Formats a given number into human friendly string representation of bytes, rounded to 2 decimal places.
func formatSize(s int64) string {
	size := float64(s)
	for _, unit := range []string{"bytes", "KiB", "MiB"} {
		if size < 1024 {
			return fmt.Sprintf("%.2f %s", size, unit)
		}
		size /= 1024
	}
	return fmt.Sprintf("%.2f GiB", size)
}

// Extract all the packages from a given blob.manifest.list and blob.sizes.
// It also returns a map containing all blobs, with the merkle root as the key.
func extractPackages(blobList, blobSize string) (blobMap map[string]*Blob, packages []string, err error) {
	blobMap = make(map[string]*Blob)

	var sizeMap map[string]int64
	if sizeMap, err = processBlobSizes(blobSize); err != nil {
		return
	}

	f, err := os.Open(blobList)
	if err != nil {
		return
	}
	defer f.Close()

	scanner := bufio.NewScanner(f)
	for scanner.Scan() {
		pkg, err := processBlobsManifest(blobMap, sizeMap, scanner.Text())
		if err != nil {
			return blobMap, packages, err
		}

		packages = append(packages, pkg...)
	}

	return
}

// Opens a blobs.manifest file to populate the blob map and extract all meta.far blobs.
// We expect each entry of blobs.manifest to have the following format:
// `$MERKLE_ROOT=$PATH_TO_BLOB`
func processBlobsManifest(
	blobMap map[string]*Blob,
	sizeMap map[string]int64,
	manifest string) ([]string, error) {
	f, err := os.Open(filepath.Join(buildDir, manifest))
	if err != nil {
		return nil, err
	}
	defer f.Close()

	return processManifest(blobMap, sizeMap, manifest, f), nil
}

// Similar to processBlobsManifest, except it doesn't throw an I/O error.
func processManifest(
	blobMap map[string]*Blob,
	sizeMap map[string]int64,
	manifest string, r io.Reader) []string {
	packages := []string{}

	scanner := bufio.NewScanner(r)
	for scanner.Scan() {
		temp := strings.Split(scanner.Text(), "=")
		if blob, ok := blobMap[temp[0]]; !ok {
			blob = &Blob{
				dep:  []string{manifest},
				name: temp[1],
				size: sizeMap[temp[0]],
				hash: temp[0],
			}

			blobMap[temp[0]] = blob
			// This blob is a Fuchsia package.
			if strings.HasSuffix(temp[1], MetaFar) {
				packages = append(packages, temp[1])
			}
		} else {
			blob.dep = append(blob.dep, manifest)
		}
	}

	return packages
}

// Translates blob.sizes into a map, with the key as the merkle root and the value as the size of
// that blob.
// We expect the format of the blob.sizes to be
// `$MERKLE_ROOT=$SIZE_IN_BYTES`
func processBlobSizes(src string) (m map[string]int64, err error) {
	f, err := os.Open(src)
	if err != nil {
		return nil, err
	}
	defer f.Close()

	return processSizes(f), nil
}

// Similar to processBlobSizes, except it doesn't throw an I/O error.
func processSizes(r io.Reader) map[string]int64 {
	m := map[string]int64{}

	scanner := bufio.NewScanner(r)
	for scanner.Scan() {
		temp := strings.Split(scanner.Text(), "=")
		sz, _ := strconv.ParseInt(temp[1], 10, 64)
		m[temp[0]] = sz
	}
	return m
}

// Process the packages extracted from blob.manifest.list and process the blobs.json file to build a
// tree of packages.
func processPackages(
	blobMap map[string]*Blob,
	assetMap map[string]bool,
	assetSize *int64,
	packages []string,
	root *Node) error {
	for _, metaFar := range packages {
		// From the meta.far file, we can get the path to the blobs.json for that package.
		dir := filepath.Dir(metaFar)
		blobJson := filepath.Join(buildDir, dir, BlobsJSON)
		// We then parse the blobs.json
		blobs := []BlobFromJSON{}
		data, err := ioutil.ReadFile(blobJson)
		if err != nil {
			return fmt.Errorf(readError(blobJson, err))
		}
		if err := json.Unmarshal(data, &blobs); err != nil {
			return fmt.Errorf(unmarshalError(blobJson, err))
		}
		// Finally, we add the blob and the package to the tree.
		processBlobsJSON(blobMap, assetMap, assetSize, blobs, root, dir)
	}
	return nil
}

// Similar to processPackages except it doesn't throw an I/O error.
func processBlobsJSON(
	blobMap map[string]*Blob,
	assetMap map[string]bool,
	assetSize *int64,
	blobs []BlobFromJSON,
	root *Node,
	pkgPath string) {
	for _, blob := range blobs {
		// If the blob is an asset, we don't add it to the tree.
		// We check the path instead of the source path because prebuilt packages have hashes as the
		// source path for their blobs
		if assetMap[filepath.Ext(blob.Path)] {
			// The size of each blob is the total space occupied by the blob in blobfs (each blob may be
			// referenced several times by different packages). Therefore, once we have already add the
			// size, we should remove it from the map
			if blobMap[blob.Merkle] != nil {
				*assetSize += blobMap[blob.Merkle].size
				delete(blobMap, blob.Merkle)
			}
		} else {
			root.add(pkgPath, blobMap[blob.Merkle])
		}
	}
}

// Processes the given input and throws an error if any component in the input is above its
// allocated space limit.
func processInput(input *Input, blobList, blobSize string) error {
	blobMap, packages, err := extractPackages(blobList, blobSize)
	if err != nil {
		return err
	}

	// We create a set of extensions that should be considered as assets.
	assetMap := make(map[string]bool)
	for _, ext := range input.AssetExt {
		assetMap[ext] = true
	}
	// The dummy node will have the root node as its only child.
	dummy := newDummyNode()
	var assetSize int64
	// We process the meta.far files that were found in the blobs.manifest here.
	if err := processPackages(blobMap, assetMap, &assetSize, packages, dummy); err != nil {
		return fmt.Errorf("error processing packages: %s", err)
	}

	var total int64
	var noSpace = false
	var report strings.Builder
	for _, component := range input.Components {
		var size int64
		for _, src := range component.Src {
			node := dummy.find(src)
			if node == nil {
				return fmt.Errorf("cannot find a folder called %s", src)
			}
			size += node.size
		}
		total += size

		if s := checkLimit(component.Component, size, component.Limit); s != "" {
			noSpace = true
			report.WriteString(s + "\n")
		}
	}

	if s := checkLimit("Assets (Fonts / Strings / Images)", assetSize, input.AssetLimit); s != "" {
		noSpace = true
		report.WriteString(s + "\n")
	}

	var root *Node
	// The dummy should only have a single child, which is the root directory.
	for _, node := range dummy.children {
		root = node
	}

	if s := checkLimit("Core system+services", root.size-total, input.CoreLimit); s != "" {
		noSpace = true
		report.WriteString(s + "\n")
	}

	if noSpace {
		return fmt.Errorf(report.String())
	}

	return nil
}

// Checks a given component to see if its size is greater than its allocated space limit as defined
// in the input JSON file.
func checkLimit(name string, size int64, limit json.Number) string {
	l, err := limit.Int64()
	if err != nil {
		log.Fatalf("failed to parse %s as an int64: %s\n", limit, err)
	}

	if size > l {
		return fmt.Sprintf("%s (%s) had exceeded its allocated limit of %s.", name, formatSize(size), formatSize(l))
	}

	return ""
}

func readError(file string, err error) string {
	return verbError("read", file, err)
}

func unmarshalError(file string, err error) string {
	return verbError("unmarshal", file, err)
}

func verbError(verb, file string, err error) string {
	return fmt.Sprintf("Failed to %s %s: %s", verb, file, err)
}

var inputJSON string
var buildDir string

func main() {
	flag.StringVar(&inputJSON, "input_json", "", `input JSON to the size checker.
The JSON file should consist of the following keys:
  asset_ext: a list of extensions that should be considered as assets.
  asset_limit: maximum size (in bytes) allocated for the assets.
  core_limit: maximum size (in bytes) allocated for the core system and/or services.
  components: a list of component objects. Each object should contain the following keys:
    component: name of the component.
    src: name of the area / package to be included as part of the component.
    limit: maximum size (in bytes) allocated for the component`)
	flag.StringVar(&buildDir, "build_dir", "", `(required) the output directory of a Fuchsia build.`)
	flag.Parse()

	if inputJSON == "" || buildDir == "" {
		flag.PrintDefaults()
		os.Exit(2)
	}

	inputData, err := ioutil.ReadFile(inputJSON)
	if err != nil {
		log.Fatal(readError(inputJSON, err))
	}
	var input Input
	if err := json.Unmarshal(inputData, &input); err != nil {
		log.Fatal(unmarshalError(inputJSON, err))
	}
	// If there are no components, then there are no work to do. We are done.
	if len(input.Components) == 0 {
		os.Exit(0)
	}

	if err := processInput(&input, filepath.Join(buildDir, BlobList), filepath.Join(buildDir, BlobSizes)); err != nil {
		log.Fatal(err)
	}
}
