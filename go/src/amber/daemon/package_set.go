// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package daemon

import (
	"fmt"
	"sync"
)

// PackageSet is a concurrency-safe set collection of Packages
type PackageSet struct {
	mu   sync.Mutex
	pkgs []*Package
}

// NewPackageSet initializes a PackageSet struct
func NewPackageSet() *PackageSet {
	return &PackageSet{
		pkgs: []*Package{},
		mu:   sync.Mutex{},
	}
}

// Add adds a package to our list of packages.
func (r *PackageSet) Add(pkg *Package) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.pkgs = append(r.pkgs, pkg)
}

// Remove removes a package from our list of packages.
func (r *PackageSet) Remove(pkg *Package) error {
	r.mu.Lock()
	defer r.mu.Unlock()

	for i := range r.pkgs {
		if *r.pkgs[i] == *pkg {
			r.pkgs = append(r.pkgs[:i], r.pkgs[i+1:]...)
			return nil
		}
	}

	return fmt.Errorf("Requested package not found")
}

// Packages returns copy of the list of Packages
func (r *PackageSet) Packages() []*Package {
	r.mu.Lock()
	c := make([]*Package, len(r.pkgs))
	copy(c, r.pkgs)
	r.mu.Unlock()
	return c
}
