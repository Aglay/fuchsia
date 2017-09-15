// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package node

import (
	"fuchsia.googlesource.com/thinfs/lib/fs/msdosfs/bootrecord"
	"fuchsia.googlesource.com/thinfs/lib/fs/msdosfs/cluster"
	"fuchsia.googlesource.com/thinfs/lib/thinio"
)

// Metadata describes the important, shared metadata of the filesystem.
type Metadata struct {
	Dev        *thinio.Conductor      // Access to device (with cache).
	Br         *bootrecord.Bootrecord // Superblock of filesystem
	ClusterMgr *cluster.Manager
	Readonly   bool

	Dcache
}

// Init initializes metadata structure
func (m *Metadata) Init() {
	m.Dcache.Init()
}
