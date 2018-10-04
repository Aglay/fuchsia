// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Package pkgfs hosts a filesystem for interacting with packages that are
// stored on a host. It presents a tree of packages that are locally available
// and a tree that enables a user to add new packages and/or package content to
// the host.
package pkgfs

import (
	"fmt"
	"io"
	"log"
	"os"
	"path"
	"path/filepath"
	"runtime"
	"sync"
	"syscall"
	"syscall/zx"
	"syscall/zx/fidl"
	zxio "syscall/zx/io"
	"time"

	"app/context"
	"fidl/fuchsia/amber"
	"thinfs/fs"
	"thinfs/zircon/rpc"

	"fuchsia.googlesource.com/pmd/blobfs"
	"fuchsia.googlesource.com/pmd/index"
)

// Filesystem is the top level container for a pkgfs server
type Filesystem struct {
	root      *rootDirectory
	static    *index.StaticIndex
	gc        *collector
	index     *index.DynamicIndex
	blobfs    *blobfs.Manager
	mountInfo mountInfo
	mountTime time.Time
	amberPxy  *amber.ControlInterface
}

// New initializes a new pkgfs filesystem server
func New(indexDir, blobDir string) (*Filesystem, error) {
	bm, err := blobfs.New(blobDir)
	if err != nil {
		return nil, fmt.Errorf("pkgfs: open blobfs: %s", err)
	}

	static := index.NewStatic()
	f := &Filesystem{
		static: static,
		index:  index.NewDynamic(indexDir, static),
		blobfs: bm,
		mountInfo: mountInfo{
			parentFd: -1,
		},
	}

	f.root = &rootDirectory{
		unsupportedDirectory: unsupportedDirectory("/"),
		fs:                   f,

		dirs: map[string]fs.Directory{
			"install": &installDir{
				unsupportedDirectory: unsupportedDirectory("/install"),
				fs:                   f,
			},
			"needs": &needsRoot{
				unsupportedDirectory: unsupportedDirectory("/needs"),
				fs:                   f,
			},
			"packages": &packagesRoot{
				unsupportedDirectory: unsupportedDirectory("/packages"),
				fs:                   f,
			},
			"system":   unsupportedDirectory("/system"),
			"metadata": unsupportedDirectory("/metadata"),
		},
	}

	req, pxy, err := amber.NewControlInterfaceRequest()
	if err != nil {
		panic(err.Error())
	}
	context.CreateFromStartupInfo().ConnectToEnvService(req)
	f.amberPxy = pxy

	f.index.Notifier = pxy

	f.gc = &collector{
		fs:       f,
		dynIndex: f.index,
		blobfs:   bm,
		root:     f.root,
	}

	return f, nil
}

// staticIndexPath is the path inside the system package directory that contains the static packages for that system version.
const staticIndexPath = "data/static_packages"

// loadStaticIndex loads the blob specified by root from blobfs. A non-nil
// *StaticIndex is always returned. If an error is returned that indicates a
// problem reading the index content from disk and therefore the StaticIndex
// returned may be empty.
func loadStaticIndex(static *index.StaticIndex, blobfs *blobfs.Manager, root string) error {
	indexFile, err := blobfs.Open(root)
	if err != nil {
		return fmt.Errorf("pkgfs: could not load static index from blob %s: %s", root, err)
	}
	defer indexFile.Close()

	return static.LoadFrom(indexFile)
}

func readBlobfs(blobfs *blobfs.Manager) (map[string]struct{}, error) {
	dnames, err := readDir(blobfs.Root)
	if err != nil {
		log.Printf("pkgfs: error reading(%q): %s", blobfs.Root, err)
		// Note: translates to zx.ErrBadState
		return nil, fs.ErrFailedPrecondition
	}
	names := make(map[string]struct{})
	for _, name := range dnames {
		names[name] = struct{}{}
	}
	return names, nil
}

func readDir(path string) ([]string, error) {
	d, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer d.Close()
	return d.Readdirnames(-1)
}

// SetSystemRoot sets/updates the merkleroot (and static index) that backs the /system partition and static package index.
func (f *Filesystem) SetSystemRoot(merkleroot string) error {
	pd, err := newPackageDirFromBlob(merkleroot, f)
	if err != nil {
		return err
	}
	f.root.setDir("system", pd)

	blob, ok := pd.getBlobFor(staticIndexPath)
	if !ok {
		return fmt.Errorf("pkgfs: new system root set, but new static index %q not found in %q", staticIndexPath, merkleroot)
	}

	err = loadStaticIndex(f.static, f.blobfs, blob)

	if err == nil {
		f.gc.staticIdx = blob
	} else {
		f.gc.staticIdx = ""
	}
	return err
}

func (f *Filesystem) Blockcount() int64 {
	// TODO(raggi): sum up all packages?
	// TODO(raggi): delegate to blobfs?
	debugLog("fs blockcount")
	return 0
}

func (f *Filesystem) Blocksize() int64 {
	// TODO(raggi): sum up all packages?
	// TODO(raggi): delegate to blobfs?
	debugLog("fs blocksize")
	return 0
}

func (f *Filesystem) Size() int64 {
	debugLog("fs size")
	// TODO(raggi): delegate to blobfs?
	return 0
}

func (f *Filesystem) Close() error {
	f.Unmount()
	return nil
}

func (f *Filesystem) RootDirectory() fs.Directory {
	return f.root
}

func (f *Filesystem) Type() string {
	return "pkgfs"
}

func (f *Filesystem) FreeSize() int64 {
	return 0
}

func (f *Filesystem) DevicePath() string {
	return ""
}

// Serve starts a Directory protocol RPC server on the given channel.
func (f *Filesystem) Serve(c zx.Channel) error {
	// rpc.NewServer takes ownership of the Handle and will close it on error.
	vfs, err := rpc.NewServer(f, zx.Handle(c))
	if err != nil {
		return fmt.Errorf("vfs server creation: %s", err)
	}
	f.mountInfo.serveChannel = c

	// TODO(jmatt) find a better time to run GC. Ideally GC would be fast
	// enough that it could be done during startup, but testing indicates that
	// it requires 1-2 seconds on hardware. Possibly this is related to the
	// amount of I/O required, further investigation is clearly indicated.
	go func() {
		time.Sleep(15 * time.Second)
		if err := f.gc.GC(); err != nil {
			log.Printf("pkgfs: GC error: %s", err)
		}
	}()

	// TODO(raggi): serve has no quit/shutdown path.
	for i := runtime.NumCPU(); i > 1; i-- {
		go vfs.Serve()
	}
	vfs.Serve()
	return nil
}

// Mount attaches the filesystem host to the given path. If an error occurs
// during setup, this method returns that error. If an error occurs after
// serving has started, the error causes a log.Fatal. If the given path does not
// exist, it is created before mounting.
func (f *Filesystem) Mount(path string) error {
	err := os.MkdirAll(path, os.ModePerm)
	if err != nil {
		return err
	}

	f.mountInfo.parentFd, err = syscall.Open(path, syscall.O_ADMIN|syscall.O_DIRECTORY, 0777)
	if err != nil {
		return err
	}

	var rpcChan, mountChan zx.Channel
	rpcChan, mountChan, err = zx.NewChannel(0)
	if err != nil {
		syscall.Close(f.mountInfo.parentFd)
		f.mountInfo.parentFd = -1
		return fmt.Errorf("channel creation: %s", err)
	}

	remote := zxio.DirectoryInterface(fidl.InterfaceRequest{Channel: mountChan})
	dirChan := zx.Channel(syscall.FDIOForFD(f.mountInfo.parentFd).Handles()[0])
	dir := zxio.DirectoryAdminInterface(fidl.InterfaceRequest{Channel: dirChan})
	if status, err := dir.Mount(remote); err != nil || status != zx.ErrOk {
		rpcChan.Close()
		syscall.Close(f.mountInfo.parentFd)
		f.mountInfo.parentFd = -1
		return fmt.Errorf("mount failure: %s", err)
	}

	return f.Serve(rpcChan)
}

// Unmount detaches the filesystem from a previously mounted path. If mount was not previously called or successful, this will panic.
func (f *Filesystem) Unmount() {
	f.mountInfo.unmountOnce.Do(func() {
		// parentFd is -1 in the case where f was just Serve()'d instead of Mount()'d
		if f.mountInfo.parentFd != -1 {
			dirChan := zx.Channel(syscall.FDIOForFD(f.mountInfo.parentFd).Handles()[0])
			dir := zxio.DirectoryAdminInterface(fidl.InterfaceRequest{Channel: dirChan})
			dir.UnmountNode()
			syscall.Close(f.mountInfo.parentFd)
			f.mountInfo.parentFd = -1
		}
		f.mountInfo.serveChannel.Close()
		f.mountInfo.serveChannel = 0
	})
}

var _ fs.FileSystem = (*Filesystem)(nil)

// clean canonicalizes a path and returns a path that is relative to an assumed root.
// as a result of this cleaning operation, an open of '/' or '.' or '' all return ''.
// TODO(raggi): speed this up/reduce allocation overhead.
func clean(path string) string {
	return filepath.Clean("/" + path)[1:]
}

type mountInfo struct {
	unmountOnce  sync.Once
	serveChannel zx.Channel
	parentFd     int
}

func goErrToFSErr(err error) error {
	switch e := err.(type) {
	case nil:
		return nil
	case *os.PathError:
		return goErrToFSErr(e.Err)
	case zx.Error:
		switch e.Status {
		case zx.ErrNotFound:
			return fs.ErrNotFound

		default:
			debugLog("pkgfs: unmapped os err to fs err: %T %v", err, err)
			return err

		}
	}
	switch err {
	case os.ErrInvalid:
		return fs.ErrInvalidArgs
	case os.ErrPermission:
		return fs.ErrPermission
	case os.ErrExist:
		return fs.ErrAlreadyExists
	case os.ErrNotExist:
		return fs.ErrNotFound
	case os.ErrClosed:
		return fs.ErrNotOpen
	case io.EOF:
		return fs.ErrEOF
	default:
		debugLog("pkgfs: unmapped os err to fs err: %T %v", err, err)
		return err
	}
}

type collector struct {
	staticIdx string
	fs        *Filesystem
	dynIndex  *index.DynamicIndex
	blobfs    *blobfs.Manager
	root      *rootDirectory
}

// GC examines the static and dynamic indexes, collects all the blobs that
// belong to packages in these indexes. It then reads blobfs for its entire
// list of blobs. Anything in blobfs that does not appear in the indexes is
// removed.
func (c *collector) GC() error {
	log.Println("pkgfs: running GC")
	c.root.Lock()
	defer c.root.Unlock()

	// First find everything used by the dynamic index
	// Next find everything used by packages being installed
	// Then find everything needed by the static index
	// Finally find everything the system package needs

	// get all the meta FAR blobs from the dynamic index
	dPkgs, err := c.dynIndex.PackageBlobs()
	if err != nil {
		log.Printf("pkgfs: error getting package blobs from dynamic index: %s", err)
		dPkgs = []string{}
	}

	// get directories for all the items in the dynamic index and collect all
	// their blobs, which include the meta FAR itself
	dynamicBlobs := make(map[string]struct{})
	for _, pkg := range dPkgs {
		pDir, err := newPackageDirFromBlob(pkg, c.fs)
		if err != nil {
			log.Printf("pkgfs: failed getting package from blob %s: %s", pkg, err)
		}

		for _, p := range pDir.Blobs() {
			dynamicBlobs[p] = struct{}{}
		}
	}

	// find all the blobs belong to packages being installed
	reserved := c.dynIndex.InstallingBlobs()

	// get all the meta FAR blobs from the static index
	// the index is created from the on-disk rather than using the instance in
	// memory because the in-memory one is actually mutable, and therefore can
	// be "wrong"
	if c.staticIdx == "" {
		return fmt.Errorf("pkgfs: static index not set, aborting GC")
	}
	staticIndex := index.NewStatic()
	err = loadStaticIndex(staticIndex, c.blobfs, c.staticIdx)
	if err != nil {
		return fmt.Errorf("pkgfs: static index failed to load: %s", err)
	}

	sPkgs := staticIndex.PackageBlobs()

	// get all the blobs for the package
	staticBlobs := make(map[string]struct{})
	for _, pkg := range sPkgs {
		pDir, err := newPackageDirFromBlob(pkg, c.fs)
		if err != nil {
			return fmt.Errorf("pkgfs: failed reading package for static index entry %s: %s",
				pkg, err)
		}

		for _, p := range pDir.Blobs() {
			staticBlobs[p] = struct{}{}
		}
	}

	// get the system directory and its blobs
	sysBlobs := []string{}
	sd := c.root.dirLocked("system")
	if sysDir, ok := sd.(*packageDir); ok && sysDir != nil {
		sysBlobs = sysDir.Blobs()
	} else {
		return fmt.Errorf("pkgfs: failure reading system_image package, nil or unknown type")
	}

	// add the blobs for the system package to the one for the static index
	// since they are all part of the "verified" set
	for _, b := range sysBlobs {
		staticBlobs[b] = struct{}{}
	}

	// for potentially interesting debugging purposes, see what things appear
	// only in the dynamic index. This is not required for proper function, but
	// is included as a sanity check during early deployment.
	dUnique := []string{}
	for blob, _ := range dynamicBlobs {
		if _, ok := staticBlobs[blob]; !ok {
			dUnique = append(dUnique, blob)
		}
	}

	log.Printf("pkgfs: %d blobs in dynamic index are not in static index", len(dUnique))
	log.Printf("pkgfs: system package backed by %d blobs", len(sysBlobs))
	log.Printf("pkgfs: %d blobs in verified software set", len(staticBlobs))

	// get the set of blobs currently in blobfs
	installedBlobs, err := readBlobfs(c.blobfs)
	if err != nil {
		return fmt.Errorf("pkgfs: unable to list blobfs: %s", err)
	}
	log.Printf("pkgfs: %d blobs in blobfs", len(installedBlobs))

	// remove the blobs for the dynamic index
	for b := range dynamicBlobs {
		delete(installedBlobs, b)
	}

	// remove blobs for installs in progress
	for _, b := range reserved {
		delete(installedBlobs, b)
	}

	// remove static index and system package blobs
	for b := range staticBlobs {
		delete(installedBlobs, b)
	}

	// remove all the blobs we no longer need
	log.Printf("pkgfs: removing %d blobs from blobfs", len(installedBlobs))
	for targ := range installedBlobs {
		e := os.Remove(path.Join("/blob", targ))
		if e != nil {
			log.Printf("pkgfs: error removing %s from blobfs: %s\n", targ, e)
		}
	}
	return nil
}
