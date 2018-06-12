// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package symbolize

import (
	"bufio"
	"bytes"
	"encoding/hex"
	"fmt"
	"os"
	"strings"
	"sync"

	"fuchsia.googlesource.com/tools/elflib"
)

// TODO(jakehehrlich): Document what this is for.
// TODO(jakehehrlich): Consider giving this a more descriptive name.
type Source interface {
	// The name of this source which is usually the path to its backing file.
	Name() string
	// Extracts the set of binaries from this source.
	GetBinaries() ([]Binary, error)
}

type buildIDError struct {
	err      error
	filename string
}

func newBuildIDError(err error, filename string) *buildIDError {
	return &buildIDError{err: err, filename: filename}
}

func (b buildIDError) Error() string {
	return fmt.Sprintf("error reading %s: %v", b.filename, b.err)
}

// TODO(jakehehrlich): Document what this is for.
// TODO(jakehehrlich): Consider giving this a more accurate name.
type Binary struct {
	BuildID string
	Name    string
}

// Verify ensures this Binary's build ID is derived from its contents.
func (b *Binary) Verify() error {
	filename := b.Name
	build := b.BuildID

	file, err := os.Open(filename)
	if err != nil {
		return newBuildIDError(err, filename)
	}
	buildIDs, err := elflib.GetBuildIDs(filename, file)
	if err != nil {
		return newBuildIDError(err, filename)
	}
	binBuild, err := hex.DecodeString(build)
	if err != nil {
		return newBuildIDError(fmt.Errorf("build ID `%s` is not a hex string: %v", build, err), filename)
	}
	for _, buildID := range buildIDs {
		if bytes.Equal(buildID, binBuild) {
			return nil
		}
	}
	return newBuildIDError(fmt.Errorf("build ID `%s` could not be found", build), filename)
}

// TODO(jakehehrlich): Document what this is for.
// TODO(jakehehrlich): Consider giving this a more descriptive name.
type IDsSource struct {
	pathToIDs string
}

func NewIDsSource(pathToIDs string) Source {
	return &IDsSource{pathToIDs}
}

func (i *IDsSource) Name() string {
	return i.pathToIDs
}

func (i *IDsSource) GetBinaries() ([]Binary, error) {
	file, err := os.Open(i.pathToIDs)
	if err != nil {
		return nil, err
	}
	scanner := bufio.NewScanner(file)
	out := []Binary{}
	for line := 0; scanner.Scan(); line++ {
		parts := strings.SplitN(scanner.Text(), " ", 2)
		if len(parts) != 2 {
			return nil, fmt.Errorf("error parsing %s at line %d", i.pathToIDs, line)
		}
		build := parts[0]
		filename := parts[1]
		out = append(out, Binary{Name: filename, BuildID: build})
	}
	return out, nil
}

type buildInfo struct {
	filepath string
	buildID  string
}

// SymbolizerRepo keeps track of build objects and source files used in those build objects.
type SymbolizerRepo struct {
	lock    sync.RWMutex
	sources []Source
	builds  map[string]*buildInfo
}

func (s *SymbolizerRepo) AddObject(build, filename string) {
	s.lock.Lock()
	defer s.lock.Unlock()
	s.builds[build] = &buildInfo{
		filepath: filename,
		buildID:  build,
	}
}

func NewRepo() *SymbolizerRepo {
	return &SymbolizerRepo{
		builds: make(map[string]*buildInfo),
	}
}

func (s *SymbolizerRepo) loadSource(source Source) error {
	bins, err := source.GetBinaries()
	if err != nil {
		return err
	}
	// Verify each binary
	for _, bin := range bins {
		if err := bin.Verify(); err != nil {
			return err
		}
	}
	// TODO: Do this in parallel
	for _, bin := range bins {
		s.AddObject(bin.BuildID, bin.Name)
	}
	return nil
}

// AddSource adds a source of binaries and all contained binaries.
func (s *SymbolizerRepo) AddSource(source Source) error {
	s.sources = append(s.sources, source)
	return s.loadSource(source)
}

func (s *SymbolizerRepo) reloadSources() error {
	for _, source := range s.sources {
		if err := s.loadSource(source); err != nil {
			return err
		}
	}
	return nil
}

func (s *SymbolizerRepo) readInfo(buildid string) (*buildInfo, bool) {
	s.lock.RLock()
	info, ok := s.builds[buildid]
	s.lock.RUnlock()
	return info, ok
}

// GetBuildObject lets you get the build object associated with a build ID.
func (s *SymbolizerRepo) GetBuildObject(buildid string) (string, error) {
	// No defer is used here because we don't want to hold the read lock
	// for very long.
	info, ok := s.readInfo(buildid)
	if !ok {
		// If we don't recognize that build ID, reload all sources.
		s.reloadSources()
		info, ok = s.readInfo(buildid)
		if !ok {
			// If we still don't know about that build, return an error.
			return "", fmt.Errorf("unrecognized build ID %s", buildid)
		}
	}
	return info.filepath, nil
}
