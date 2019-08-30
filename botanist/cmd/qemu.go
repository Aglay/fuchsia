// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package main

import (
	"context"
	"flag"
	"fmt"

	"github.com/google/subcommands"
	"go.fuchsia.dev/tools/botanist/target"
	"go.fuchsia.dev/tools/build"
	"go.fuchsia.dev/tools/logger"
)

// QEMUBinPrefix is the prefix of the QEMU binary name, which is of the form
// qemu-system-<QEMU arch suffix>.
const qemuBinPrefix = "qemu-system"

// QEMUCommand is a Command implementation for running the testing workflow on an emulated
// target within QEMU.
type QEMUCommand struct {
	// ImageManifest is the path an image manifest specifying a QEMU kernel, a zircon
	// kernel, and block image to back QEMU storage.
	imageManifest string

	// QEMUBinDir is a path to a directory of QEMU binaries.
	qemuBinDir string

	// MinFSImage is a path to a minFS image to be mounted on target, and to where test
	// results will be written.
	minFSImage string

	// MinFSBlkDevPCIAddr is a minFS block device PCI address.
	minFSBlkDevPCIAddr string

	// TargetArch is the target architecture to be emulated within QEMU
	targetArch string

	// EnableKVM dictates whether to enable KVM.
	enableKVM bool

	// CPU is the number of processors to emulate.
	cpu int

	// Memory is the amount of memory (in MB) to provide.
	memory int
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
	f.StringVar(&cmd.imageManifest, "images", "", "path to an image manifest")
	f.StringVar(&cmd.qemuBinDir, "qemu-dir", "", "")
	f.StringVar(&cmd.minFSImage, "minfs", "", "path to minFS image")
	f.StringVar(&cmd.minFSBlkDevPCIAddr, "pci-addr", "06.0", "minFS block device PCI address")
	f.StringVar(&cmd.targetArch, "arch", "", "target architecture (x64 or arm64)")
	f.BoolVar(&cmd.enableKVM, "use-kvm", false, "whether to enable KVM")
	f.IntVar(&cmd.cpu, "cpu", 4, "number of processors to emulate")
	f.IntVar(&cmd.memory, "memory", 4096, "amount of memory (in MB) to provide")
}

func (cmd *QEMUCommand) execute(ctx context.Context, cmdlineArgs []string) error {
	if cmd.qemuBinDir == "" {
		return fmt.Errorf("-qemu-dir must be set")
	}

	imgs, err := build.LoadImages(cmd.imageManifest)
	if err != nil {
		return err
	}

	// TODO: pass this directly from a file.
	config := target.QEMUConfig{
		CPU:            cmd.cpu,
		Memory:         cmd.memory,
		Path:           cmd.qemuBinDir,
		Target:         cmd.targetArch,
		KVM:            cmd.enableKVM,
		UserNetworking: true,
	}
	if cmd.minFSImage != "" {
		config.MinFS = &target.MinFS{
			Image:      cmd.minFSImage,
			PCIAddress: cmd.minFSBlkDevPCIAddr,
		}
	}

	t := target.NewQEMUTarget(config, target.Options{})
	if err := t.Start(ctx, imgs, cmdlineArgs); err != nil {
		return err
	}

	return t.Wait(ctx)
}

func (cmd *QEMUCommand) Execute(ctx context.Context, f *flag.FlagSet, _ ...interface{}) subcommands.ExitStatus {
	if err := cmd.execute(ctx, f.Args()); err != nil {
		logger.Errorf(ctx, "%v\n", err)
		return subcommands.ExitFailure
	}
	return subcommands.ExitSuccess
}
