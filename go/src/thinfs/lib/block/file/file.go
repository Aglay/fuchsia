// Copyright 2016 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Package file implements the block.Device interface backed by a traditional file.
package file

import (
	"fmt"
	"os"

	"github.com/golang/glog"
)

const (
	defaultBlockSize int64 = 512
)

// File represents a block device backed by a file on a traditional file system.
type File struct {
	f         *os.File
	info      os.FileInfo
	size      int64
	blockSize int64
}

func getSize(f *os.File, info os.FileInfo) int64 {
	if info.Mode()&os.ModeDevice != 0 {
		if size, err := ioctlBlockGetSize(f.Fd()); err == nil {
			return size
		}
	}

	// If the file is a block device but the ioctl failed for some reason or if the file is a
	// regular file, just fall back to using the size reported by Stat().
	return info.Size()
}

// New creates and returns a new File, using f as the backing store.  The size of the
// block device represented by the returned File will be the size of f.  New will
// not close f if any errors occur.
func New(f *os.File) (*File, error) {
	info, err := f.Stat()
	if err != nil {
		return nil, &os.PathError{
			Op:   "New",
			Path: f.Name(),
			Err:  err,
		}
	}

	size := getSize(f, info)
	if glog.V(2) {
		glog.Info("File name: ", info.Name())
		glog.Info("     size: ", size)
		glog.Info("     mode: ", info.Mode())
	}

	return &File{
		f:         f,
		info:      info,
		size:      size,
		blockSize: defaultBlockSize,
	}, nil
}

// BlockSize returns the size in bytes of the smallest block that can be written by the File.
// The return value is undefined after Close() is called.
func (f *File) BlockSize() int64 {
	return f.blockSize
}

// Size returns the fixed size of the File in bytes.  The return value is undefined after Close()
// is called.
func (f *File) Size() int64 {
	return f.size
}

func (f *File) check(p []byte, off int64, op string) error {
	if off%f.blockSize != 0 {
		return &os.PathError{
			Op:   op,
			Path: f.info.Name(),
			Err:  fmt.Errorf("off (%v) is not a multiple of blocksize", off),
		}
	}

	if int64(len(p))%f.blockSize != 0 {
		return &os.PathError{
			Op:   op,
			Path: f.info.Name(),
			Err:  fmt.Errorf("len(p) (%v) is not a multiple of blocksize", len(p)),
		}
	}

	if off+int64(len(p)) > f.Size() {
		return &os.PathError{
			Op:   op,
			Path: f.info.Name(),
			Err:  fmt.Errorf("the requested range [%v, %v) is out of bounds", off, off+int64(len(p))),
		}
	}

	return nil
}

// ReadAt reads len(p) bytes from the device starting at offset off.  Both off and len(p) must
// be multiples of BlockSize().  It returns the number of bytes read and the error, if any.
// ReadAt always returns a non-nil error when n < len(p).
func (f *File) ReadAt(p []byte, off int64) (n int, err error) {
	if err := f.check(p, off, "ReadAt"); err != nil {
		return 0, err
	}

	if glog.V(2) {
		glog.Infof("ReadAt: reading %v bytes from offset %#x\n", len(p), off)
	}

	return f.f.ReadAt(p, off)
}

// WriteAt writes the contents of p to the devices starting at offset off.  Both off and len(p) must
// be multiples of BlockSize().  It returns the number of bytes written and an error, if any.
// WriteAt always returns a non-nil error when n < len(p).
func (f *File) WriteAt(p []byte, off int64) (n int, err error) {
	if err := f.check(p, off, "WriteAt"); err != nil {
		return 0, err
	}

	if glog.V(2) {
		glog.Infof("WriteAt: writing %v bytes to address %#x\n", len(p), off)
	}

	return f.f.WriteAt(p, off)
}

// Flush forces any writes that have been cached in memory to be committed to persistent storage.
// Returns an error, if any.
func (f *File) Flush() error {
	if glog.V(2) {
		glog.Infof("Syncing file %s\n", f.info.Name())
	}

	return f.f.Sync()
}

// Discard marks the address range [off, off+size) as being unused, allowing it to be reclaimed by
// the device for other purposes.  Both off and size must be multiples of Blocksize().  Returns an
// error, if any.
func (f *File) Discard(off, len int64) error {
	if glog.V(2) {
		glog.Infof("Discarding data in range [%v, %v)\n", off, off+len)
	}

	if f.info.Mode()&os.ModeDevice != 0 {
		return ioctlBlockDiscard(f.f.Fd(), uint64(off), uint64(len))
	}

	return fallocate(f.f.Fd(), off, len)
}

// Close calls Flush() and then closes the device, rendering it unusable for I/O.  It returns an error,
// if any.
func (f *File) Close() error {
	if err := f.Flush(); err != nil {
		return err
	}

	if glog.V(2) {
		glog.Infof("Closing file %s\n", f.info.Name())
	}

	return f.f.Close()
}
