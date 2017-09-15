// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// +build fuchsia

package file

import (
	"syscall"
	"syscall/mx/fdio"
)

func ioctlBlockGetSize(fd uintptr) (int64, error) {
	return 0, syscall.EOPNOTSUPP
}

func ioctlBlockDiscard(fd uintptr, off, size uint64) error {
	return syscall.EOPNOTSUPP
}

func ioctlBlockGetSectorSize(fd uintptr) (int64, error) {
	return 0, syscall.EOPNOTSUPP
}

func fallocate(fd uintptr, off, len int64) error {
	return syscall.EOPNOTSUPP
}

func ioctlDeviceGetTopoPath(fd uintptr) string {
	m := syscall.FDIOForFD(int(fd))
	path := make([]byte, 1024)
	_, err := m.Ioctl(fdio.IoctlDeviceGetTopoPath, nil, path)

	if err == nil {
		return string(path)
	}

	return ""
}
