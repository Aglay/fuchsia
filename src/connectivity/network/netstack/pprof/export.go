// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package pprof

import (
	"bytes"
	"io/ioutil"
	"os"
	"path/filepath"
	"runtime/pprof"
	"sort"
	"sync"
	"syscall/zx"
	"time"

	appcontext "app/context"
	"netstack/inspect"
)

const pprofName = "pprof"

func Setup(path string) (appcontext.Node, func() error, error) {
	mapDir, err := mapDirFromPath(path)
	if err != nil {
		return nil, nil, err
	}

	return &appcontext.DirectoryWrapper{
			Directory: mapDir,
		}, func() error {
			t := time.NewTicker(10 * time.Minute)
			defer t.Stop()

			for {
				mapDir.mu.Lock()
				// Prune all but the most recent profiles. The number retained is
				// chosen arbitrarily.
				if maxProfiles := 3; len(mapDir.mu.m) > maxProfiles {
					filenames := make([]string, 0, len(mapDir.mu.m))
					for filename := range mapDir.mu.m {
						filenames = append(filenames, filename)
					}
					sort.Strings(filenames)
					for _, filename := range filenames[:len(filenames)-maxProfiles] {
						delete(mapDir.mu.m, filename)
					}
				}
				mapDir.mu.Unlock()
				filename := (<-t.C).UTC().Format(time.RFC3339) + ".inspect"

				b, err := getProfilesInspectVMOBytes()
				if err != nil {
					return err
				}
				mapDir.mu.Lock()
				mapDir.mu.m[filename] = &appcontext.FileWrapper{
					File: &sliceFile{
						b: b,
					},
				}
				mapDir.mu.Unlock()

				if err := ioutil.WriteFile(filepath.Join(path, filename), b, os.ModePerm); err != nil {
					return err
				}
			}
		}, nil
}

var _ appcontext.Directory = (*mapDirectory)(nil)

type mapDirectory struct {
	mu struct {
		sync.Mutex
		m map[string]appcontext.Node
	}
}

func mapDirFromPath(path string) (*mapDirectory, error) {
	dir, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	filenames, err := dir.Readdirnames(0)
	if err != nil {
		return nil, err
	}
	if err := dir.Close(); err != nil {
		return nil, err
	}

	m := make(map[string]appcontext.Node)
	for _, filename := range filenames {
		f, err := os.Open(filepath.Join(path, filename))
		if err != nil {
			return nil, err
		}
		b, err := ioutil.ReadAll(f)
		if err != nil {
			return nil, err
		}
		if err := f.Close(); err != nil {
			return nil, err
		}
		m[filename] = &appcontext.FileWrapper{
			File: &sliceFile{
				b: b,
			},
		}
	}
	var d mapDirectory
	d.mu.m = m
	return &d, nil
}

const nowName = "now.inspect"

var nowFile = appcontext.FileWrapper{File: &pprofFile{}}

func (md *mapDirectory) Get(nodeName string) (appcontext.Node, bool) {
	if nodeName == nowName {
		return &nowFile, true
	}
	md.mu.Lock()
	value, ok := md.mu.m[nodeName]
	md.mu.Unlock()
	return value, ok
}

func (md *mapDirectory) ForEach(fn func(string, appcontext.Node)) {
	fn(nowName, &nowFile)
	md.mu.Lock()
	for nodeName, node := range md.mu.m {
		fn(nodeName, node)
	}
	md.mu.Unlock()
}

var _ appcontext.File = (*sliceFile)(nil)

type sliceFile struct {
	b    []byte
	once struct {
		sync.Once
		vmo zx.VMO
	}
}

func (sf *sliceFile) GetReader() (appcontext.Reader, uint64) {
	r := bytes.NewReader(sf.b)
	return r, uint64(r.Len())
}

func (sf *sliceFile) GetVMO() zx.VMO {
	sf.once.Do(func() {
		vmo, err := zx.NewVMO(uint64(len(sf.b)), 0)
		if err != nil {
			return
		}
		if err := vmo.Write(sf.b, 0); err != nil {
			return
		}
		sf.once.vmo = vmo
	})
	return sf.once.vmo
}

var _ appcontext.File = (*pprofFile)(nil)

type pprofFile struct{}

func getProfilesInspectVMOBytes() ([]byte, error) {
	var b bytes.Buffer
	w, err := inspect.NewWriter(&b)
	if err != nil {
		return nil, err
	}
	nodeValueIndex, err := w.WriteNodeValueBlock(0, pprofName)
	if err != nil {
		return nil, err
	}
	for _, p := range pprof.Profiles() {
		var b bytes.Buffer
		if err := p.WriteTo(&b, 0); err != nil {
			return nil, err
		}
		if err := w.WriteBinary(nodeValueIndex, p.Name(), uint32(b.Len()), &b); err != nil {
			return nil, err
		}
	}
	return b.Bytes(), nil
}

func (p *pprofFile) GetReader() (appcontext.Reader, uint64) {
	b, err := getProfilesInspectVMOBytes()
	if err != nil {
		panic(err)
	}
	return bytes.NewReader(b), uint64(len(b))
}

func (*pprofFile) GetVMO() zx.VMO {
	return zx.VMO(zx.HandleInvalid)
}
