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
	"os"
	"path/filepath"
	"sync"
	"syscall"
	"syscall/zx"
	"syscall/zx/fdio"
	"time"

	"application/lib/app/context"
	"fidl/bindings"
	"garnet/amber/api/amber"
	"thinfs/fs"
	"thinfs/zircon/rpc"

	"fuchsia.googlesource.com/pmd/blobstore"
	"fuchsia.googlesource.com/pmd/index"
)

// Filesystem is the top level container for a pkgfs server
type Filesystem struct {
	root      fs.Directory
	static    *index.StaticIndex
	index     *index.DynamicIndex
	blobstore *blobstore.Manager
	mountInfo mountInfo
	mountTime time.Time
	amberPxy  *amber.Control_Proxy
}

// New initializes a new pkgfs filesystem server
func New(staticIndex, indexDir, blobstoreDir string) (*Filesystem, error) {
	static := index.NewStatic()

	if _, err := os.Stat(staticIndex); !os.IsNotExist(err) {
		err := static.LoadFrom(staticIndex)
		if err != nil {
			// TODO(raggi): avoid crashing the process in cases like this
			return nil, err
		}
	}

	index, err := index.NewDynamic(indexDir)
	if err != nil {
		return nil, err
	}

	bm, err := blobstore.New(blobstoreDir, "")
	if err != nil {
		return nil, err
	}

	f := &Filesystem{
		static:    static,
		index:     index,
		blobstore: bm,
		mountInfo: mountInfo{
			parentFd: -1,
		},
	}

	f.root = &rootDirectory{
		unsupportedDirectory: unsupportedDirectory("/"),
		fs:                   f,

		dirs: map[string]fs.Directory{
			"incoming": &inDirectory{
				unsupportedDirectory: unsupportedDirectory("/incoming"),
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
			"metadata": unsupportedDirectory("/metadata"),
		},
	}

	var pxy *amber.Control_Proxy
	req, pxy := pxy.NewRequest(bindings.GetAsyncWaiter())
	context.CreateFromStartupInfo().ConnectToEnvService(req)
	f.amberPxy = pxy

	return f, nil
}

// NewSinglePackage initializes a new pkgfs filesystem that hosts only a single
// package.
func NewSinglePackage(blob, blobstoreDir string) (*Filesystem, error) {
	bm, err := blobstore.New(blobstoreDir, "")
	if err != nil {
		return nil, err
	}

	f := &Filesystem{
		static:    nil,
		index:     nil,
		blobstore: bm,
		mountInfo: mountInfo{
			parentFd: -1,
		},
	}

	pd, err := newPackageDirFromBlob(blob, f)
	if err != nil {
		return nil, err
	}

	f.root = pd

	return f, nil
}

func (f *Filesystem) Blockcount() int64 {
	// TODO(raggi): sum up all packages?
	// TODO(raggi): delegate to blobstore?
	debugLog("fs blockcount")
	return 0
}

func (f *Filesystem) Blocksize() int64 {
	// TODO(raggi): sum up all packages?
	// TODO(raggi): delegate to blobstore?
	debugLog("fs blocksize")
	return 0
}

func (f *Filesystem) Size() int64 {
	debugLog("fs size")
	// TODO(raggi): delegate to blobstore?
	return 0
}

func (f *Filesystem) Close() error {
	debugLog("fs close")
	return fs.ErrNotSupported
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
func (f *Filesystem) Serve(c *zx.Channel) error {
	// rpc.NewServer takes ownership of the Handle and will close it on error.
	vfs, err := rpc.NewServer(f, c.Handle)
	if err != nil {
		return fmt.Errorf("vfs server creation: %s", err)
	}
	f.mountInfo.serveChannel = c

	// TODO(raggi): serve has no quit/shutdown path.
	go vfs.Serve()
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

	var rpcChan, mountChan *zx.Channel
	rpcChan, mountChan, err = zx.NewChannel(0)
	if err != nil {
		syscall.Close(f.mountInfo.parentFd)
		f.mountInfo.parentFd = -1
		return fmt.Errorf("channel creation: %s", err)
	}

	if err := syscall.FDIOForFD(f.mountInfo.parentFd).IoctlSetHandle(fdio.IoctlVFSMountFS, mountChan.Handle); err != nil {
		mountChan.Close()
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
			syscall.FDIOForFD(f.mountInfo.parentFd).Ioctl(fdio.IoctlVFSUnmountNode, nil, nil)
			syscall.Close(f.mountInfo.parentFd)
			f.mountInfo.parentFd = -1
		}
		f.mountInfo.serveChannel.Close()
		f.mountInfo.serveChannel = nil
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
	serveChannel *zx.Channel
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
