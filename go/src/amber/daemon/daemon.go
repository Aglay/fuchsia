// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package daemon

import (
	"errors"
	"fmt"
	"io"
	"os"
	"strings"
	"sync"
	"time"
)

var newTicker = time.NewTicker

// ErrSrcNotFound is returned when a request is made to RemoveSource, but the
// passed in Source is not known to the Daemon
var ErrSrcNotFound = errors.New("amber/daemon: no corresponding source found")

// Deamon provides access to a set of Sources and oversees the polling of those
// Sources.
//
// Note that methods on this struct are not designed for parallel access.
// Execution contexts sharing a single Daemon instance should mediate access
// to all calls into the Daemon.
type Daemon struct {
	srcMons   []*SourceMonitor
	pkgs      *PackageSet
	runCount  sync.WaitGroup
	processor func(*Package, Source) error
	// sources must claim this before running updates
	updateLock sync.Mutex
}

// NewDaemon creates a Daemon with the given SourceSet
func NewDaemon(r *PackageSet, f func(*Package, Source) error) *Daemon {
	return &Daemon{pkgs: r, processor: f, updateLock: sync.Mutex{}, srcMons: []*SourceMonitor{}}
}

// AddSource is called to add a Source that can be used to get updates. When the
// Source is added, the Daemon will start polling it at the interval from
// Source.GetInterval()
func (d *Daemon) AddSource(s Source) {
	mon := &SourceMonitor{src: s,
		pkgs:      d.pkgs,
		processor: d.processor,
		runGate:   &d.updateLock}
	d.srcMons = append(d.srcMons, mon)
	d.runCount.Add(1)
	go func() {
		defer d.runCount.Done()
		mon.Run()
	}()
}

// RemoveSource should be used to stop using a Source previously added with
// AddSource. This method does not wait for any in-progress polling operation
// on the Source to complete. This method returns ErrSrcNotFound if the supplied
// Source is not know to this Daemon.
func (d *Daemon) RemoveSource(src Source) error {
	for i, m := range d.srcMons {
		if m.src.Equals(src) {
			d.srcMons = append(d.srcMons[:i], d.srcMons[i+1:]...)
			m.Stop()
			return nil
		}

	}
	return ErrSrcNotFound
}

// CancelAll stops all update retrieval operations, blocking until any
// in-progress operations complete.
func (d *Daemon) CancelAll() {
	for _, s := range d.srcMons {
		s.Stop()
	}

	d.runCount.Wait()
	d.srcMons = []*SourceMonitor{}
}

// ErrProcPkgIO is a general I/O error during ProcessPackage
type ErrProcessPackage string

func NewErrProcessPackage(f string, args ...interface{}) error {
	return ErrProcessPackage(fmt.Sprintf("processor: %s", fmt.Sprintf(f, args...)))
}

func (e ErrProcessPackage) Error() string {
	return string(e)
}

// ProcessPackage attempts to retrieve the content of the supplied Package
// from the supplied Source. If retrieval from the Source fails, the Source's
// error is returned. If there is a local I/O error when processing the package
// an ErrProcPkgIO is returned.
func ProcessPackage(pkg *Package, src Source) error {
	// this package processor can only deal with names that look like
	// file paths
	// TODO(jmatt) better checking that this could be a valid file path
	if strings.Index(pkg.Name, "/") != 0 {
		return NewErrProcessPackage("invalid path")
	}

	file, e := src.FetchPkg(pkg)
	if e != nil {
		return e
	}
	defer file.Close()
	defer os.Remove(file.Name())

	// take the long way to truncate in case this is a VMO filesystem
	// which doesn't support truncate
	e = os.Remove(pkg.Name)
	if e != nil && !os.IsNotExist(e) {
		return NewErrProcessPackage("couldn't remove old file %v", e)
	}

	dst, e := os.OpenFile(pkg.Name, os.O_RDWR|os.O_CREATE, 0666)
	if e != nil {
		return NewErrProcessPackage("couldn't open file to replace %v", e)
	}
	defer dst.Close()

	_, e = io.Copy(dst, file)
	// TODO(jmatt) validate file on disk, size, hash, etc
	if e != nil {
		return NewErrProcessPackage("couldn't write update to file %v", e)
	}
	return nil
}
