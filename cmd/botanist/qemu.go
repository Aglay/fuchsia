// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"io"
	"io/ioutil"
	"log"
	"os"
	"path/filepath"
	"strings"

	"fuchsia.googlesource.com/tools/qemu"
	"fuchsia.googlesource.com/tools/secrets"
	"github.com/google/subcommands"
)

// QCOWImageName is a default name for a QEMU CoW (Copy on Write) image.
const qcowImageName = "fuchsia.qcow2"

// qemuImgTool is the name of the QEMU image utility tool.
const qemuImgTool = "qemu-img"

// QEMUBinPrefix is the prefix of the QEMU binary name, which is of the form
// qemu-system-<QEMU arch suffix>.
const qemuBinPrefix = "qemu-system"

// TargetToQEMUArch maps the fuchsia shorthand of a target architecture to the name
// recognized by QEMU.
var targetToQEMUArch = map[string]string{
	"x64":   "x86_64",
	"arm64": "aarch64",
}

func copyFile(src, dst string) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()

	out, err := os.Create(dst)
	if err != nil {
		return err
	}
	defer out.Close()

	_, err = io.Copy(out, in)
	return err
}

// OverwriteFileWithCopy overwrites a given file with a copy of it.
//
// This is used to break any hard linking that might back the provided MinFS image.
// For example, Swarming hard links cached isolate downloads - and modifying those will
// modify the cache contents themselves.
func overwriteFileWithCopy(filepath string) error {
	copy, err := ioutil.TempFile("", "file-copy")
	if err != nil {
		return err
	}
	if err = copyFile(filepath, copy.Name()); err != nil {
		return err
	}

	if err = os.Remove(filepath); err != nil {
		return err
	}

	return os.Rename(copy.Name(), filepath)
}

// QEMUCommand is a Command implementation for running the testing workflow on an emulated
// target within QEMU.
type QEMUCommand struct {
	// QEMUBinDir is a path to a directory of QEMU binaries.
	qemuBinDir string

	// QEMUKernelImage is a path to a qemu-kernel image.
	qemuKernelImage string

	// zirconAImage is a path to a zircon-a image.
	zirconAImage string

	// storageFullImage is a path to a storage-full image.
	storageFullImage string

	// MinFSImage is a path to a minFS image to be mounted on target, and to where test
	// results will be written.
	minFSImage string

	// MinFSBlkDevPCIAddr is a minFS block device PCI address.
	minFSBlkDevPCIAddr string

	// TargetArch is the target architecture to be emulated within QEMU
	targetArch string

	// EnableKVM dictates whether to enable KVM.
	enableKVM bool

	// EnableNetworking dictates whether to enable external networking.
	enableNetworking bool
}

func (*QEMUCommand) Name() string {
	return "qemu"
}

func (*QEMUCommand) Usage() string {
	return "qemu [flags...] [kernel command-line arguments...]\n\nflags:\n"
}

func (*QEMUCommand) Synopsis() string {
	return "boots a QEMU device with a MinFS image as a block device."
}

func (cmd *QEMUCommand) SetFlags(f *flag.FlagSet) {
	f.StringVar(&cmd.qemuBinDir, "qemu-dir", "", "")
	f.StringVar(&cmd.qemuKernelImage, "qemu-kernel", "", "path to a qemu-kernel image")
	f.StringVar(&cmd.zirconAImage, "zircon-a", "", "path to a zircon-a image")
	f.StringVar(&cmd.storageFullImage, "storage-full", "", "path to a storage-full image")
	f.StringVar(&cmd.minFSImage, "minfs", "", "path to minFS image")
	f.StringVar(&cmd.minFSBlkDevPCIAddr, "pci-addr", "06.0", "minFS block device PCI address")
	f.StringVar(&cmd.targetArch, "arch", "", "target architecture (x64 or arm64)")
	f.BoolVar(&cmd.enableKVM, "use-kvm", false, "whether to enable KVM")
	f.BoolVar(&cmd.enableNetworking, "enable-networking", false, "whether to enable external networking")
}

// EnsureFlagsAreSet validates that required flags are set and that the provided images exist.
func (cmd *QEMUCommand) validateFlags() error {
	var errs []string

	if cmd.qemuBinDir == "" {
		errs = append(errs, "-qemu-dir must be set")
	}
	_, ok := targetToQEMUArch[cmd.targetArch]
	if !ok {
		errs = append(errs, fmt.Sprintf("invalid target architecture: %s", cmd.targetArch))
	}

	existsIfSet := func(filename string) {
		if filename == "" {
			return
		}
		_, err := os.Stat(filename)
		if err != nil {
			errs = append(errs, fmt.Sprintf("failed to stat %s: %v", filename, err))
		}
	}

	existsIfSet(cmd.qemuKernelImage)
	if cmd.qemuKernelImage == "" {
		errs = append(errs, "-qemu-kernel must be set.")
	}
	existsIfSet(cmd.zirconAImage)
	if cmd.zirconAImage == "" {
		errs = append(errs, "-zircon-a must be set.")
	}
	existsIfSet(cmd.storageFullImage)
	existsIfSet(cmd.minFSImage)

	if len(errs) > 0 {
		errs = append(errs, "run `help qemu` for flag documentation.")
		return errors.New(strings.Join(errs, "\n"))
	}
	return nil
}

func (cmd *QEMUCommand) execute(ctx context.Context, cmdlineArgs []string) error {
	if err := cmd.validateFlags(); err != nil {
		return err
	}

	qemuArch, _ := targetToQEMUArch[cmd.targetArch]
	qemuBinPath := filepath.Join(cmd.qemuBinDir, fmt.Sprintf("%s-%s", qemuBinPrefix, qemuArch))

	var q qemu.QEMUBuilder
	if err := q.Initialize(qemuBinPath, cmd.targetArch, cmd.enableKVM); err != nil {
		return err
	}

	q.AddArgs("-m", "4096")
	q.AddArgs("-smp", "4")
	q.AddArgs("-nographic")
	q.AddArgs("-serial", "stdio")
	q.AddArgs("-monitor", "none")
	if !cmd.enableNetworking {
		q.AddArgs("-net", "none")
	}

	// The system will halt on a kernel panic instead of rebooting
	cmdlineArgs = append(cmdlineArgs, "kernel.halt-on-panic=true")
	// Print a message if `dm poweroff` times out.
	cmdlineArgs = append(cmdlineArgs, "devmgr.suspend-timeout-debug=true")
	// Do not print colors.
	cmdlineArgs = append(cmdlineArgs, "TERM=dumb")
	if cmd.targetArch == "x64" {
		// Necessary to redirect to stdout.
		cmdlineArgs = append(cmdlineArgs, "kernel.serial=legacy")
	}
	q.AddArgs("-append", strings.Join(cmdlineArgs, " "))

	// Absolutize qemuKernelImage, zirconAImage, and minFSImage paths so that QEMU can be
	// invoked in any working directory.
	absQEMUKernelImage, err := filepath.Abs(cmd.qemuKernelImage)
	if err != nil {
		return err
	}
	q.AddArgs("-kernel", absQEMUKernelImage)

	absZirconAImage, err := filepath.Abs(cmd.zirconAImage)
	if err != nil {
		return err
	}
	q.AddArgs("-initrd", absZirconAImage)

	if cmd.minFSImage != "" {
		absMinFSImage, err := filepath.Abs(cmd.minFSImage)
		if err != nil {
			return err
		}

		// See function documentation for the reason behind this step.
		if err := overwriteFileWithCopy(absMinFSImage); err != nil {
			return err
		}

		q.AddArgs("-drive", fmt.Sprintf("file=%s,format=raw,if=none,id=testdisk", absMinFSImage))
		q.AddArgs("-device", fmt.Sprintf("virtio-blk-pci,drive=testdisk,addr=%s", cmd.minFSBlkDevPCIAddr))
	}

	if cmd.storageFullImage != "" {
		qemuImgToolPath := filepath.Join(cmd.qemuBinDir, qemuImgTool)
		qcowDir, err := ioutil.TempDir("", "qcow-dir")
		if err != nil {
			return err
		}
		defer os.RemoveAll(qcowDir)
		qcowImage := filepath.Join(qcowDir, qcowImageName)
		if err = qemu.CreateQCOWImage(qemuImgToolPath, cmd.storageFullImage, qcowImage); err != nil {
			return err
		}
		qcowPath := filepath.Join(qcowDir, qcowImageName)
		q.AddArgs("-drive", fmt.Sprintf("file=%s,format=qcow2,if=none,id=maindisk", qcowPath))
		q.AddArgs("-device", "virtio-blk-pci,drive=maindisk")
	}

	// The QEMU command needs to be invoked within an empty directory, as QEMU will attempt
	// to pick up files from its working directory, one notable culprit being multiboot.bin.
	// This can result in strange behavior.
	qemuWorkingDir, err := ioutil.TempDir("", "qemu-working-dir")
	if err != nil {
		return err
	}
	defer os.RemoveAll(qemuWorkingDir)

	qemuCmd := q.Cmd(ctx)
	qemuCmd.Dir = qemuWorkingDir
	qemuCmd.Stdout = os.Stdout
	qemuCmd.Stderr = os.Stderr
	log.Printf("running:\n\tArgs: %s\n\tEnv: %s", qemuCmd.Args, qemuCmd.Env)
	return qemu.CheckExitCode(qemuCmd.Run())
}

func (cmd *QEMUCommand) Execute(ctx context.Context, f *flag.FlagSet, _ ...interface{}) subcommands.ExitStatus {
	// TODO(IN-607) Once the secrets pipeline is supported on hardware, move the starting
	// of the secrets server to botanist's main().
	//
	// The secrets server will start up iff LUCI_CONTEXT is set and contains secret bytes.
	secrets.StartSecretsServer(ctx, 8081)

	if err := cmd.execute(ctx, f.Args()); err != nil {
		log.Print(err)
		return subcommands.ExitFailure
	}
	return subcommands.ExitSuccess
}
