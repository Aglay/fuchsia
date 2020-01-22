// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
package main

import (
	"context"
	"flag"
	"fmt"
	"io"
	"io/ioutil"
	"os"
	"strings"
	"sync"
	"time"

	"go.fuchsia.dev/fuchsia/tools/bootserver/lib"
	"go.fuchsia.dev/fuchsia/tools/botanist/target"
	"go.fuchsia.dev/fuchsia/tools/lib/logger"
	"go.fuchsia.dev/fuchsia/tools/net/tftp"
	"go.fuchsia.dev/fuchsia/tools/testing/runtests"

	"github.com/google/subcommands"
)

// ZedbootCommand is a Command implementation for running the testing workflow on a device
// that boots with Zedboot.
type ZedbootCommand struct {
	// ImageManifest is a path to an image manifest
	imageManifest string

	// Netboot tells botanist to netboot (and not to pave).
	netboot bool

	// ConfigFile is the path to a file containing the target config.
	configFile string

	// TestResultsDir is the directory on target to where test results will be written.
	testResultsDir string

	// SummaryFilename is the name of the test summary JSON file to be written to
	// testResultsDir.
	summaryFilename string

	// FilePollInterval is the duration waited between checking for test summary file
	// on the target to be written.
	filePollInterval time.Duration

	// OutputArchive is a path on host to where the tarball containing the test results
	// will be output.
	outputArchive string

	// CmdlineFile is the path to a file of additional kernel command-line arguments.
	cmdlineFile string

	// SerialLogFile, if nonempty, is the file where the system's serial logs will be written.
	serialLogFile string
}

func (*ZedbootCommand) Name() string {
	return "zedboot"
}

func (*ZedbootCommand) Usage() string {
	return "zedboot [flags...] [kernel command-line arguments...]\n\nflags:\n"
}

func (*ZedbootCommand) Synopsis() string {
	return "boots a Zedboot device and collects test results"
}

func (cmd *ZedbootCommand) SetFlags(f *flag.FlagSet) {
	f.StringVar(&cmd.imageManifest, "images", "", "path to an image manifest")
	f.BoolVar(&cmd.netboot, "netboot", false, "if set, botanist will not pave; but will netboot instead")
	f.StringVar(&cmd.testResultsDir, "results-dir", "/test", "path on target to where test results will be written")
	f.StringVar(&cmd.outputArchive, "out", "output.tar", "path on host to output tarball of test results")
	f.StringVar(&cmd.summaryFilename, "summary-name", runtests.TestSummaryFilename, "name of the file in the test directory")
	f.DurationVar(&cmd.filePollInterval, "poll-interval", 1*time.Minute, "time between checking for summary.json on the target")
	f.StringVar(&cmd.configFile, "config", "", "path to file of device config")
	f.StringVar(&cmd.cmdlineFile, "cmdline-file", "", "path to a file containing additional kernel command-line arguments")
	f.StringVar(&cmd.serialLogFile, "serial-log", "", "file to write the serial logs to.")
}

func (cmd *ZedbootCommand) runTests(ctx context.Context, t tftp.Client, cmdlineArgs []string) error {
	logger.Debugf(ctx, "waiting for %q\n", cmd.summaryFilename)
	return runtests.PollForSummary(ctx, t, cmd.summaryFilename, cmd.testResultsDir, cmd.outputArchive, cmd.filePollInterval)
}

func (cmd *ZedbootCommand) execute(ctx context.Context, cmdlineArgs []string) error {
	configs, err := target.LoadDeviceConfigs(cmd.configFile)

	if err != nil {
		return fmt.Errorf("failed to load target config file %q", cmd.configFile)
	}

	var bootMode bootserver.Mode
	if cmd.netboot {
		bootMode = bootserver.ModeNetboot
	} else {
		bootMode = bootserver.ModePave
	}
	imgs, closeFunc, err := bootserver.GetImages(ctx, cmd.imageManifest, bootMode)
	if err != nil {
		return err
	}
	defer closeFunc()
	opts := target.Options{
		Netboot: cmd.netboot,
	}

	var devices []*target.DeviceTarget
	for _, config := range configs {
		device, err := target.NewDeviceTarget(ctx, config, opts)
		if err != nil {
			return err
		}
		devices = append(devices, device)
	}

	ctx, cancel := context.WithCancel(ctx)
	defer cancel()
	errs := make(chan error)

	if serial := devices[0].Serial(); serial != nil && cmd.serialLogFile != "" {
		defer serial.Close()

		// Modify the zirconArgs passed to the kernel on boot to enable serial on x64.
		// arm64 devices should already be enabling kernel.serial at compile time.
		cmdlineArgs = append(cmdlineArgs, "kernel.serial=legacy")
		// Force serial output to the console instead of buffering it.
		cmdlineArgs = append(cmdlineArgs, "kernel.bypass-debuglog=true")

		serialLog, err := os.Create(cmd.serialLogFile)
		if err != nil {
			return err
		}
		defer serialLog.Close()
		// Here we invoke the `dlog` command over serial to tail the existing log buffer into the
		// output file.  This should give us everything since Zedboot boot, and new messages should
		// be written to directly to the serial port without needing to tail with `dlog -f`.
		if _, err = io.WriteString(serial, "\ndlog\n"); err != nil {
			return fmt.Errorf("failed to tail zedboot dlog: %v", err)
		}
		go func() {
			if _, err := io.Copy(serialLog, serial); err != nil {
				errs <- fmt.Errorf("failed to write serial log: %v", err)
			}
		}()
	}

	// Defer asynchronously restarts of each device.
	defer func() {
		var wg sync.WaitGroup
		for _, device := range devices {
			wg.Add(1)
			go func(device *target.DeviceTarget) {
				defer wg.Done()
				device.Restart(ctx)
			}(device)
		}
		wg.Wait()
	}()

	var wg sync.WaitGroup
	for _, device := range devices {
		wg.Add(1)
		go func(device *target.DeviceTarget) {
			defer wg.Done()
			if err := device.Start(ctx, imgs, cmdlineArgs); err != nil {
				errs <- err
			}
		}(device)
	}

	go func() {
		wg.Wait()
		// We execute tests here against the 0th device, there may be N devices
		// in the test bed but all other devices are driven by the tests not
		// the test runner.
		errs <- cmd.runTests(ctx, devices[0].Tftp(), cmdlineArgs)
	}()

	select {
	case err := <-errs:
		return err
	case <-ctx.Done():
		return nil
	}
}

func (cmd *ZedbootCommand) Execute(ctx context.Context, f *flag.FlagSet, _ ...interface{}) subcommands.ExitStatus {
	configFlag := f.Lookup("config")
	logger.Debugf(ctx, "config flag: %v\n", configFlag.Value)

	// Aggregate command-line arguments.
	cmdlineArgs := f.Args()
	if cmd.cmdlineFile != "" {
		args, err := ioutil.ReadFile(cmd.cmdlineFile)
		if err != nil {
			logger.Errorf(ctx, "failed to read command-line args file %q: %v\n", cmd.cmdlineFile, err)
			return subcommands.ExitFailure
		}
		cmdlineArgs = append(cmdlineArgs, strings.Split(string(args), "\n")...)
	}

	if err := cmd.execute(ctx, cmdlineArgs); err != nil {
		logger.Errorf(ctx, "%v\n", err)
		return subcommands.ExitFailure
	}

	return subcommands.ExitSuccess
}
