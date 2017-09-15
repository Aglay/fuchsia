// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package filesystem

import (
	"path"
	"strings"

	mojoerr "interfaces/errors"
	"interfaces/filesystem/common"
	"interfaces/filesystem/directory"
	mojofile "interfaces/filesystem/file"
	"mojo/public/go/bindings"
	"mojo/public/go/system"

	"fuchsia.googlesource.com/thinfs/lib/fs"
	"github.com/golang/glog"
	"github.com/pkg/errors"
)

// dir holds a pointer to an inode and any connection-specific state.
type dir struct {
	fs    *filesystem
	dir   fs.Directory
	flags common.OpenFlags
}

func convertError(err error) (mojoerr.Error, error) {
	switch errors.Cause(err) {
	case fs.ErrAlreadyExists:
		return mojoerr.Error_AlreadyExists, nil
	case fs.ErrNotFound:
		return mojoerr.Error_NotFound, nil
	case fs.ErrNotADir:
		return mojoerr.Error_FailedPrecondition, nil
	case fs.ErrNotAFile:
		return mojoerr.Error_FailedPrecondition, nil
	case fs.ErrNotEmpty:
		return mojoerr.Error_FailedPrecondition, nil
	case fs.ErrIsActive:
		return mojoerr.Error_FailedPrecondition, nil
	default:
		return mojoerr.Error_Internal, err
	}
}

func serveDirectory(fs *filesystem, vdir fs.Directory, req directory.Directory_Request, flags common.OpenFlags) {
	d := &dir{
		fs:    fs,
		dir:   vdir,
		flags: flags,
	}
	stub := directory.NewDirectoryStub(req, d, bindings.GetAsyncWaiter())

	go func() {
		var err error
		for err == nil {
			err = stub.ServeRequest()
		}

		connErr, ok := err.(*bindings.ConnectionError)
		if !ok || !connErr.Closed() {
			// Log any error that's not a connection closed error.
			glog.Error(err)
		}

		d.fs.Lock()
		if err := d.dir.Close(); err != nil {
			glog.Error(err)
		}
		d.fs.Unlock()

		// The recount should have been incremented before serveDirectory was called.
		d.fs.decRef()
	}()
}

func convertFileType(ft fs.FileType) directory.FileType {
	switch ft {
	case fs.FileTypeDirectory:
		return directory.FileType_Directory
	case fs.FileTypeRegularFile:
		return directory.FileType_RegularFile
	default:
		return directory.FileType_Unknown
	}
}

func (d *dir) Read() (*[]directory.DirectoryEntry, mojoerr.Error, error) {
	if glog.V(2) {
		glog.Info("Read")
	}

	d.fs.Lock()
	entries, err := d.dir.Read()
	d.fs.Unlock()

	if err != nil {
		return nil, mojoerr.Error_Internal, err
	}

	out := make([]directory.DirectoryEntry, len(entries))
	for i, entry := range entries {
		out[i] = directory.DirectoryEntry{
			Type: convertFileType(entry.GetType()),
			Name: entry.GetName(),
		}
	}
	return &out, mojoerr.Error_Ok, nil
}

func (d *dir) ReadTo(src system.ProducerHandle) (mojoerr.Error, error) {
	if glog.V(2) {
		glog.Info("ReadTo: src=%\n", src)
	}

	d.fs.Lock()
	entries, err := d.dir.Read()
	d.fs.Unlock()

	if err != nil {
		return mojoerr.Error_Internal, err
	}

	go func() {
		for _, e := range entries {
			dirent := &directory.DirectoryEntry{
				Type: convertFileType(e.GetType()),
				Name: e.GetName(),
			}
			encoder := bindings.NewEncoder()
			if err := dirent.Encode(encoder); err != nil {
				glog.Errorf("Unable to encode directory entry with name=%s, type=%v: %v\n",
					dirent.Name, dirent.Type, err)
				return
			}

			data, _, err := encoder.Data()
			if err != nil {
				glog.Errorf("Unable to fetch encoded data for directory entry with name=%s, type=%v: %v\n",
					dirent.Name, dirent.Type, err)
				return
			}

			for len(data) > 0 {
				res, p := src.BeginWriteData(system.MOJO_WRITE_DATA_FLAG_NONE)
				if res != system.MOJO_RESULT_OK {
					glog.Error("Unable to begin 2-phase write: ", err)
					return
				}
				n := copy(p, data)
				res = src.EndWriteData(n)
				if res != system.MOJO_RESULT_OK {
					glog.Error("Unable to complete 2-phase write: ", err)
					return
				}
				data = data[n:]
			}
		}
	}()

	return mojoerr.Error_Ok, nil
}

func (d *dir) checkFlags(flags common.OpenFlags) (common.OpenFlags, mojoerr.Error) {
	ro := flags&common.OpenFlags_ReadOnly != 0
	rw := flags&common.OpenFlags_ReadWrite != 0

	if ro && rw {
		return 0, mojoerr.Error_InvalidArgument
	}

	var outFlags common.OpenFlags
	// The only flags we care about are read-only and read-write.
	if ro {
		// Dropping to read-only is always allowed.
		outFlags = common.OpenFlags_ReadOnly
	} else if rw {
		if d.flags&common.OpenFlags_ReadWrite == 0 {
			// Cannot request read-write permission if the client doesn't already have it.
			return 0, mojoerr.Error_PermissionDenied
		}

		outFlags = common.OpenFlags_ReadWrite
	} else {
		// Either the read-only or the read-write flag must be provided.
		return 0, mojoerr.Error_InvalidArgument
	}

	return outFlags, mojoerr.Error_Ok
}

func (d *dir) OpenFile(filepath string, req mojofile.File_Request, flags common.OpenFlags) (mojoerr.Error, error) {
	if glog.V(2) {
		glog.Infof("OpenFile: filepath=%s, req=%v, flags=%v\n", filepath, req, flags)
	}

	d.fs.Lock()
	defer d.fs.Unlock()

	cleanpath := path.Clean(filepath)
	if cleanpath == "." || strings.HasPrefix(cleanpath, "../") {
		return mojoerr.Error_InvalidArgument, nil
	}

	connFlags, flagerr := d.checkFlags(flags)
	if flagerr != mojoerr.Error_Ok {
		return flagerr, nil
	}

	var oflags fs.OpenFlags
	if flags&common.OpenFlags_Create != 0 {
		oflags |= fs.OpenFlagCreate
	}
	if flags&common.OpenFlags_Exclusive != 0 {
		oflags |= fs.OpenFlagExclusive
	}

	newfile, err := d.dir.OpenFile(cleanpath, oflags)
	if err != nil {
		return convertError(err)
	}

	// Increment the refcount now to avoid a race condition where the refcount goes to
	// zero before the new handle has a chance to increment it.
	d.fs.refcnt++
	serveFile(d.fs, newfile, req, connFlags)
	return mojoerr.Error_Ok, nil
}

func (d *dir) OpenDirectory(dirpath string, req directory.Directory_Request, flags common.OpenFlags) (mojoerr.Error, error) {
	if glog.V(2) {
		glog.Infof("OpenDirectory: dirpath=%s, req=%v, flags=%v\n", dirpath, req, flags)
	}

	d.fs.Lock()
	defer d.fs.Unlock()

	cleanPath := path.Clean(dirpath)
	if cleanPath == "." || strings.HasPrefix(cleanPath, "../") {
		return mojoerr.Error_InvalidArgument, nil
	}

	connFlags, flagerr := d.checkFlags(flags)
	if flagerr != mojoerr.Error_Ok {
		return flagerr, nil
	}

	var oflags fs.OpenFlags
	if flags&common.OpenFlags_Create != 0 {
		oflags |= fs.OpenFlagCreate
	}
	if flags&common.OpenFlags_Exclusive != 0 {
		oflags |= fs.OpenFlagExclusive
	}

	newdir, err := d.dir.OpenDirectory(cleanPath, oflags)
	if err != nil {
		return convertError(err)
	}

	// Increment the refcount now to avoid a race condition where the refcount goes to
	// zero before the new handle has a chance to increment it.
	d.fs.refcnt++
	serveDirectory(d.fs, newdir, req, connFlags)
	return mojoerr.Error_Ok, nil
}

func (d *dir) Rename(from string, to string) (mojoerr.Error, error) {
	if glog.V(2) {
		glog.Infof("Rename: from=%s, to=%s\n", from, to)
	}

	d.fs.Lock()
	defer d.fs.Unlock()

	if d.flags&common.OpenFlags_ReadWrite != 0 {
		return mojoerr.Error_PermissionDenied, nil
	}

	if err := d.dir.Rename(path.Clean(from), path.Clean(to)); err != nil {
		return convertError(err)
	}
	return mojoerr.Error_Ok, nil
}

func (d *dir) Barrier(_ *[]string) (mojoerr.Error, error) {
	if glog.V(2) {
		glog.Info("Barrier")
	}

	d.fs.Lock()
	defer d.fs.Unlock()

	if d.flags&common.OpenFlags_ReadWrite != 0 {
		return mojoerr.Error_PermissionDenied, nil
	}

	if err := d.dir.Flush(); err != nil {
		return convertError(err)
	}
	return mojoerr.Error_Ok, nil
}

func (d *dir) Delete(name string) (mojoerr.Error, error) {
	if glog.V(2) {
		glog.Infof("Delete: path=%s\n", name)
	}

	d.fs.Lock()
	defer d.fs.Unlock()

	if d.flags&common.OpenFlags_ReadWrite != 0 {
		return mojoerr.Error_PermissionDenied, nil
	}

	if err := d.dir.Unlink(path.Clean(name)); err != nil {
		return convertError(err)
	}
	return mojoerr.Error_Ok, nil
}

func (d *dir) Clone(req directory.Directory_Request) (mojoerr.Error, error) {
	return d.Reopen(req, d.flags)
}

func (d *dir) Reopen(req directory.Directory_Request, flags common.OpenFlags) (mojoerr.Error, error) {
	if glog.V(2) {
		glog.Infof("Reopen: req=%v, flags=%v\n", req, flags)
	}

	d.fs.Lock()
	defer d.fs.Unlock()

	connFlags, flagerr := d.checkFlags(flags)
	if flagerr != mojoerr.Error_Ok {
		return flagerr, nil
	}

	newdir, err := d.dir.OpenDirectory(".", 0)
	if err != nil {
		return convertError(err)
	}

	// Increment the refcount now to avoid a race condition where the refcount goes to
	// zero before the new handle has a chance to increment it.
	d.fs.refcnt++
	serveDirectory(d.fs, newdir, req, connFlags)
	return mojoerr.Error_Ok, nil
}
