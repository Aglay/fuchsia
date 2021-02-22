// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package main

import (
	"os"
	"path/filepath"
	"testing"

	"go.fuchsia.dev/fuchsia/tools/emulator"
	"go.fuchsia.dev/fuchsia/tools/emulator/emulatortest"
)

func TestKernelLockupDetectorCriticalSection(t *testing.T) {
	exDir := execDir(t)
	distro := emulatortest.UnpackFrom(t, filepath.Join(exDir, "test_data"), emulator.DistributionParams{
		Emulator: emulator.Qemu,
	})
	arch := distro.TargetCPU()
	device := emulator.DefaultVirtualDevice(string(arch))

	// Enable the lockup detector.
	//
	// Upon booting run "k", which will print a usage message.  By waiting for the usage
	// message, we can be sure the system has booted and is ready to accept "k"
	// commands.
	device.KernelArgs = append(device.KernelArgs, "kernel.lockup-detector.critical-section-threshold-ms=500", "zircon.autorun.boot=/boot/bin/sh+-c+k")
	d := distro.Create(device)

	// Boot.
	d.Start()

	// Wait for the system to finish booting.
	d.WaitForLogMessage("usage: k <command>")

	// Force two lockups and see that an OOPS is emitted for each one.
	//
	// Why force two lockups?  Because emitting an OOPS will call back into the lockup detector,
	// we want to verify that doing so does not mess up the lockup detector's state and prevent
	// subsequent events from being detected.
	for i := 0; i < 2; i++ {
		d.RunCommand("k lockup test 1 600")
		d.WaitForLogMessage("locking up CPU")
		d.WaitForLogMessage("ZIRCON KERNEL OOPS")
		d.WaitForLogMessage("CPU-1 in critical section for")
		d.WaitForLogMessage("done")
	}
}

func TestKernelLockupDetectorHeartbeat(t *testing.T) {
	exDir := execDir(t)
	distro := emulatortest.UnpackFrom(t, filepath.Join(exDir, "test_data"), emulator.DistributionParams{
		Emulator: emulator.Qemu,
	})
	arch := distro.TargetCPU()
	device := emulator.DefaultVirtualDevice(string(arch))
	device.KernelArgs = append(device.KernelArgs,
		// Enable the lockup detector.
		//
		// Upon booting run "k", which will print a usage message.  By waiting for the usage
		// message, we can be sure the system has booted and is ready to accept "k"
		// commands.
		"kernel.lockup-detector.heartbeat-period-ms=50",
		"kernel.lockup-detector.heartbeat-age-threshold-ms=200",
		"zircon.autorun.boot=/boot/bin/sh+-c+k",
	)
	d := distro.Create(device)

	// Boot.
	d.Start()

	// Wait for the system to finish booting.
	d.WaitForLogMessage("usage: k <command>")

	// Force a lockup and see that a heartbeat OOPS is emitted.
	d.RunCommand("k lockup test 1 1000")
	d.WaitForLogMessage("locking up CPU")
	d.WaitForLogMessage("ZIRCON KERNEL OOPS")
	d.WaitForLogMessage("no heartbeat from CPU-1")
	// See that the CPU's run queue is printed and contains the thread named "lockup-spin", the
	// one responsible for the lockup.
	d.WaitForLogMessage("lockup-spin")
	d.WaitForLogMessage("done")
}

func execDir(t *testing.T) string {
	ex, err := os.Executable()
	if err != nil {
		t.Fatal(err)
		return ""
	}
	return filepath.Dir(ex)
}
