// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// +build fuchsia

package main

import (
	"errors"
	"flag"
	"fmt"
	"os"

	fileBlk "fuchsia.googlesource.com/thinfs/lib/block/file"

	"fuchsia.googlesource.com/thinfs/lib/fs"
	"fuchsia.googlesource.com/thinfs/lib/fs/msdosfs"

	"fuchsia.googlesource.com/thinfs/lib/magenta/rpc"
)

var devicePathPtr = flag.String("devicepath", "", "Path to Block Device")
var mountPathPtr = flag.String("mountpath", "", "Path to Mounted Filesystem")

func parseArgs() (string, error) {
	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, "Usage:\n")
		fmt.Fprintf(os.Stderr, "	> %s [flags] {mount,unmount}\n", os.Args[0])
		flag.PrintDefaults()
	}
	flag.Parse()
	if len(flag.Args()) != 1 {
		return "", errors.New("Invalid number of arguments")
	}

	switch flag.Arg(0) {
	case "mount":
		if *devicePathPtr == "" || *mountPathPtr == "" {
			return "", errors.New("Mount requires block device path and mount path")
		}
		return "mount", nil
	case "unmount":
		if *mountPathPtr == "" {
			return "", errors.New("Unmount requires path to mount point")
		}
		return "unmount", nil
	default:
		return "", errors.New("Invalid action")
	}
}

func main() {
	action, err := parseArgs()
	if err != nil {
		println("Error: ", err.Error())
		os.Exit(1)
	}

	switch action {
	case "mount":
		// Open the block device
		f, err := os.OpenFile(*devicePathPtr, os.O_RDWR, 0666)
		if err != nil {
			println("Failed to open block device: ", err.Error())
			os.Exit(1)
		}
		dev, err := fileBlk.New(f, 512)
		if err != nil {
			println("Failed to create block device object: ", err.Error())
			os.Exit(1)
		}

		// Start the target filesystem (FAT)
		filesys, err := msdosfs.New("Thinfs FAT", dev, fs.ReadWrite)
		if err != nil {
			println("Failed to create FAT fs: ", err.Error())
			os.Exit(1)
		}

		// Mount the filesystem
		err = rpc.StartServer(*mountPathPtr, filesys)
		filesys.Close()
	case "unmount":
		// TODO(smklein): Support unmount
		println("Unmount is not yet supported")
		os.Exit(1)
	default:
		println("Unsupported arg")
		os.Exit(1)
	}
}
