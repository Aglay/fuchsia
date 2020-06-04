// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package main

import (
	"testing"

	"go.fuchsia.dev/fuchsia/src/testing/qemu"
	"go.fuchsia.dev/fuchsia/src/tests/disable_syscalls/support"
)

func TestInvalidSerialSetting(t *testing.T) {
	distro, err := qemu.Unpack()
	if err != nil {
		t.Fatal(err)
	}
	defer distro.Delete()
	arch, err := distro.TargetCPU()
	if err != nil {
		t.Fatal(err)
	}

	stdout, stderr, err := distro.RunNonInteractive(
		"/boot/bin/syscall-check",
		support.ToolPath(t, "minfs"),
		support.ToolPath(t, "zbi"),
		qemu.Params{
			Arch:          arch,
			ZBI:           support.ZbiPath(t),
			AppendCmdline: "kernel.enable-serial-syscalls=badvalue",
			// This test uses additional memory on ASAN builds than normal.
			Memory: 3072,
		})
	if err != nil {
		t.Fatal(err)
	}

	support.EnsureContains(t, stdout, "zx_debug_read: disabled")
	support.EnsureContains(t, stdout, "zx_debug_write: disabled")

	if stderr != "" {
		t.Fatal(stderr)
	}
}
