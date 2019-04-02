// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package target

import (
	"context"
	"fmt"
	"io"
	"io/ioutil"
	"log"
	"net"
	"os"
	"os/exec"
	"path/filepath"

	"fuchsia.googlesource.com/tools/build"
	"fuchsia.googlesource.com/tools/qemu"
)

const (
	// qemuSystemPrefix is the prefix of the QEMU binary name, which is of the
	// form qemu-system-<QEMU arch suffix>.
	qemuSystemPrefix = "qemu-system"

	// DefaultInterfaceName is the name given to the emulated tap interface.
	defaultInterfaceName = "qemu"

	// DefaultMACAddr is the default MAC address given to a QEMU target.
	defaultMACAddr = "52:54:00:63:5e:7a"

	// DefaultNodename is the default nodename given to an target with the default QEMU MAC address.
	defaultNodename = "step-atom-yard-juicy"
)

// qemuTargetMapping maps the Fuchsia target name to the name recognized by QEMU.
var qemuTargetMapping = map[string]string{
	"x64":   qemu.TargetX86_64,
	"arm64": qemu.TargetAArch64,
}

// MinFS is the configuration for the MinFS filesystem image.
type MinFS struct {
	// Image is the path to the filesystem image.
	Image string `json:"image"`

	// PCIAddress is the PCI address to map the device at.
	PCIAddress string `json:"pci_address"`
}

// QEMUConfig is a QEMU configuration.
type QEMUConfig struct {
	// Path is a path to a directory that contains QEMU system binary.
	Path string `json:"path"`

	// Target is the QEMU target to emulate.
	Target string `json:"target"`

	// CPU is the number of processors to emulate.
	CPU int `json:"cpu"`

	// Memory is the amount of memory (in MB) to provide.
	Memory int `json:"memory"`

	// KVM specifies whether to enable hardware virtualization acceleration.
	KVM bool `json:"kvm"`

	// MinFS is the filesystem to mount as a device.
	MinFS *MinFS `json:"minfs,omitempty"`
}

// QEMUTarget is a QEMU target.
type QEMUTarget struct {
	config QEMUConfig
	opts   Options

	c chan error

	cmd    *exec.Cmd
	status error
}

// NewQEMUTarget returns a new QEMU target with a given configuration.
func NewQEMUTarget(config QEMUConfig, opts Options) *QEMUTarget {
	return &QEMUTarget{
		config: config,
		opts:   opts,
		c:      make(chan error),
	}
}

// Nodename returns the name of the target node.
func (t *QEMUTarget) Nodename() string {
	return defaultNodename
}

// IPv4Addr returns a nil address, as DHCP is not currently configured.
func (t *QEMUTarget) IPv4Addr() (net.IP, error) {
	return nil, nil
}

// SSHKey returns the private SSH key path associated with the authorized key to be pavet.
func (t *QEMUTarget) SSHKey() string {
	return t.opts.SSHKey
}

// Start starts the QEMU target.
func (t *QEMUTarget) Start(ctx context.Context, images build.Images, args []string) error {
	qemuTarget, ok := qemuTargetMapping[t.config.Target]
	if !ok {
		return fmt.Errorf("invalid target %q", t.config.Target)
	}

	if t.config.Path == "" {
		return fmt.Errorf("directory must be set")
	}
	qemuSystem := filepath.Join(t.config.Path, fmt.Sprintf("%s-%s", qemuSystemPrefix, qemuTarget))
	if _, err := os.Stat(qemuSystem); err != nil {
		return fmt.Errorf("could not find qemu-system binary %q: %v", qemuSystem, err)
	}

	qemuKernel := images.Get("qemu-kernel")
	if qemuKernel == nil {
		return fmt.Errorf("could not find qemu-kernel")
	}
	zirconA := images.Get("zircon-a")
	if zirconA == nil {
		return fmt.Errorf("could not find zircon-a")
	}

	var drives []qemu.Drive
	if storageFull := images.Get("storage-full"); storageFull != nil {
		drives = append(drives, qemu.Drive{
			ID:   "maindisk",
			File: storageFull.Path,
		})
	}
	if t.config.MinFS != nil {
		if _, err := os.Stat(t.config.MinFS.Image); err != nil {
			return fmt.Errorf("could not find minfs image %q: %v", t.config.MinFS.Image, err)
		}
		file, err := filepath.Abs(t.config.MinFS.Image)
		if err != nil {
			return err
		}
		// Swarming hard-links Isolate downloads with a cache and the very same
		// cached minfs image will be used across multiple tasks. To ensure
		// that it remains blank, we must break its link.
		if err := overwriteFileWithCopy(file); err != nil {
			return err
		}
		drives = append(drives, qemu.Drive{
			ID:   "testdisk",
			File: file,
			Addr: t.config.MinFS.PCIAddress,
		})
	}

	networks := []qemu.Netdev{
		qemu.Netdev{
			ID: "net0",
			Tap: &qemu.NetdevTap{
				Name: defaultInterfaceName,
			},
			MAC: defaultMACAddr,
		},
	}

	config := qemu.Config{
		Binary:   qemuSystem,
		Target:   qemuTarget,
		CPU:      t.config.CPU,
		Memory:   t.config.Memory,
		KVM:      t.config.KVM,
		Kernel:   qemuKernel.Path,
		Initrd:   zirconA.Path,
		Drives:   drives,
		Networks: networks,
	}

	// The system will halt on a kernel panic instead of rebooting.
	args = append(args, "kernel.halt-on-panic=true")
	// Print a message if `dm poweroff` times out.
	args = append(args, "devmgr.suspend-timeout-debug=true")
	// Do not print colors.
	args = append(args, "TERM=dumb")
	if t.config.Target == "x64" {
		// Necessary to redirect to stdout.
		args = append(args, "kernel.serial=legacy")
	}

	invocation, err := qemu.CreateInvocation(config, args)
	if err != nil {
		return err
	}

	// The QEMU command needs to be invoked within an empty directory, as QEMU
	// will attempt to pick up files from its working directory, one notable
	// culprit being multiboot.bin.  This can result in strange behavior.
	workdir, err := ioutil.TempDir("", "qemu-working-dir")
	if err != nil {
		return err
	}

	t.cmd = &exec.Cmd{
		Path:   invocation[0],
		Args:   invocation,
		Dir:    workdir,
		Stdout: os.Stdout,
		Stderr: os.Stderr,
	}
	log.Printf("QEMU invocation:\n%s", invocation)

	if err := t.cmd.Start(); err != nil {
		os.RemoveAll(workdir)
		return fmt.Errorf("failed to start: %v", err)
	}

	// Ensure that the working directory when QEMU finishes whether the Wait
	// method is invoked or not.
	go func() {
		defer os.RemoveAll(workdir)
		t.c <- qemu.CheckExitCode(t.cmd.Wait())
	}()

	return nil
}

// Wait waits for the QEMU target to stop.
func (t *QEMUTarget) Restart(ctx context.Context) error {
	return ErrUnimplemented
}

// Stop stops the QEMU target.
func (t *QEMUTarget) Stop(ctx context.Context) error {
	return t.cmd.Process.Kill()
}

// Wait waits for the QEMU target to stop.
func (t *QEMUTarget) Wait(ctx context.Context) error {
	return <-t.c
}

func overwriteFileWithCopy(path string) error {
	tmpfile, err := ioutil.TempFile(filepath.Dir(path), "botanist")
	if err != nil {
		return err
	}
	defer tmpfile.Close()
	if err := copyFile(path, tmpfile.Name()); err != nil {
		return err
	}
	return os.Rename(tmpfile.Name(), path)
}

func copyFile(src, dest string) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	info, err := in.Stat()
	if err != nil {
		return err
	}
	out, err := os.OpenFile(dest, os.O_WRONLY|os.O_CREATE, info.Mode().Perm())
	if err != nil {
		return err
	}
	defer out.Close()
	_, err = io.Copy(out, in)
	return err
}
