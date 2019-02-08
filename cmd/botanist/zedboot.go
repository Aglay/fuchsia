// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
package main

import (
	"archive/tar"
	"bytes"
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"io/ioutil"
	"net"
	"os"
	"os/exec"
	"os/signal"
	"path"
	"path/filepath"
	"strings"
	"syscall"
	"time"

	"fuchsia.googlesource.com/tools/botanist"
	"fuchsia.googlesource.com/tools/build"
	"fuchsia.googlesource.com/tools/logger"
	"fuchsia.googlesource.com/tools/netboot"
	"fuchsia.googlesource.com/tools/pdu"
	"fuchsia.googlesource.com/tools/retry"
	"fuchsia.googlesource.com/tools/runtests"
	"fuchsia.googlesource.com/tools/tftp"
	"github.com/google/subcommands"
)

// ZedbootCommand is a Command implementation for running the testing workflow on a device
// that boots with Zedboot.
type ZedbootCommand struct {
	// ImageManifests is a list of paths to image manifests (e.g., images.json)
	imageManifests botanist.StringsFlag

	// Netboot tells botanist to netboot (and not to pave).
	netboot bool

	// PropertiesFile is the path to a file where deviceProperties have been written.
	propertiesFile string

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

	// Fastboot is a path to the fastboot tool. If set, botanist will flash
	// the device into zedboot.
	fastboot string

	// Host command to run after paving device
	// TODO(IN-831): Remove when host-target-interaction infra is ready
	hostCmd string
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
	f.Var(&cmd.imageManifests, "images", "paths to image manifests")
	f.BoolVar(&cmd.netboot, "netboot", false, "if set, botanist will not pave; but will netboot instead")
	f.StringVar(&cmd.testResultsDir, "results-dir", "/test", "path on target to where test results will be written")
	f.StringVar(&cmd.outputArchive, "out", "output.tar", "path on host to output tarball of test results")
	f.StringVar(&cmd.summaryFilename, "summary-name", runtests.TestSummaryFilename, "name of the file in the test directory")
	f.DurationVar(&cmd.filePollInterval, "poll-interval", 1*time.Minute, "time between checking for summary.json on the target")
	f.StringVar(&cmd.propertiesFile, "properties", "/etc/botanist/config.json", "path to file of device properties")
	f.StringVar(&cmd.cmdlineFile, "cmdline-file", "", "path to a file containing additional kernel command-line arguments")
	f.StringVar(&cmd.fastboot, "fastboot", "", "path to the fastboot tool; if set, the device will be flashed into Zedboot. A zircon-r must be supplied via -images")
	f.StringVar(&cmd.hostCmd, "hacky-host-cmd", "", "host command to run after paving. To be removed on completion of IN-831")
}

// Creates and returns archive file handle.
func (cmd *ZedbootCommand) createTarFile() (*os.File, error) {
	file, err := os.OpenFile(cmd.outputArchive, os.O_WRONLY|os.O_CREATE, 0666)
	if err != nil {
		return nil, fmt.Errorf("failed to create file %s: %v\n", cmd.outputArchive, err)
	}

	return file, nil
}

// Creates and returns Summary file object for Host Cmds.
func (cmd *ZedbootCommand) hostSummaryJSON(ctx context.Context, err error) (*bytes.Buffer, error) {
	var cmdResult runtests.TestResult

	if err != nil {
		cmdResult = runtests.TestFailure
		logger.Infof(ctx, "Command failed! %v\n", err)
	} else {
		cmdResult = runtests.TestSuccess
		logger.Infof(ctx, "Command succeeded!\n")
	}

	// Create coarse-grained summary based on host command exit code
	testDetail := runtests.TestDetails{
		Name:       cmd.hostCmd,
		OutputFile: runtests.TestOutputFilename,
		Result:     cmdResult,
	}

	result := runtests.TestSummary{
		Tests: []runtests.TestDetails{testDetail},
	}

	b, err := json.Marshal(result)
	if err != nil {
		return nil, err
	}
	buffer := bytes.NewBuffer(b)

	return buffer, nil
}

// Creates tar archive from host command artifacts.
func (cmd *ZedbootCommand) tarHostCmdArtifacts(summary []byte, cmdOutput []byte, outputDir string) error {
	outFile, err := cmd.createTarFile()
	if err != nil {
		return err
	}

	tw := tar.NewWriter(outFile)
	defer tw.Close()

	// Write summary to archive
	if err = botanist.ArchiveBuffer(tw, summary, cmd.summaryFilename); err != nil {
		return err
	}

	// Write combined stdout & stderr output to archive
	if err = botanist.ArchiveBuffer(tw, cmdOutput, runtests.TestOutputFilename); err != nil {
		return err
	}

	// Write all output files from the host cmd to the archive.
	return botanist.ArchiveDirectory(tw, outputDir)
}

// Executes host command and creates result tar from command output
func (cmd *ZedbootCommand) runHostCmd(ctx context.Context) error {
	// Create tmp directory to run host command out of
	tmpDir, err := ioutil.TempDir("", "output")
	if err != nil {
		return err
	}
	defer os.RemoveAll(tmpDir)

	// Define multiwriters so cmd outputs to both stdout/stderr and respective buffers
	// This allows users to see output on the fly while also storing results
	var stdoutBuf, stderrBuf bytes.Buffer
	stdoutWriter := io.MultiWriter(os.Stdout, &stdoutBuf)
	stderrWriter := io.MultiWriter(os.Stdout, &stderrBuf)

	// Define host command
	hostCmd := exec.CommandContext(ctx, cmd.hostCmd)
	hostCmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
	hostCmd.Dir = tmpDir
	hostCmd.Stdout = stdoutWriter
	hostCmd.Stderr = stderrWriter

	// Set timeout to clean up all subprocesses of original cmd
	time.AfterFunc(60*time.Minute, func() {
		syscall.Kill(-hostCmd.Process.Pid, syscall.SIGKILL)
	})

	// Execute host command
	logger.Debugf(ctx, "executing command: %q\n", cmd.hostCmd)
	hostCmd.Start()
	hostCmdErr := hostCmd.Wait()

	// Concatentate stderr after stdout
	stdoutBuf.Write(stderrBuf.Bytes())

	// Create summary JSON based on host command exit code
	summaryBuffer, err := cmd.hostSummaryJSON(ctx, hostCmdErr)
	if err != nil {
		return err
	}

	// Create tar archive
	return cmd.tarHostCmdArtifacts(summaryBuffer.Bytes(), stdoutBuf.Bytes(), tmpDir)
}

func (cmd *ZedbootCommand) runTests(ctx context.Context, imgs build.Images, nodename string, cmdlineArgs []string) error {
	// Set up log listener and dump kernel output to stdout.
	l, err := netboot.NewLogListener(nodename)
	if err != nil {
		return fmt.Errorf("cannot listen: %v\n", err)
	}
	go func() {
		defer l.Close()
		logger.Debugf(ctx, "starting log listener\n")
		for {
			data, err := l.Listen()
			if err != nil {
				continue
			}
			fmt.Print(data)
			select {
			case <-ctx.Done():
				return
			default:
			}
		}
	}()

	addr, err := botanist.GetNodeAddress(ctx, nodename)
	if err != nil {
		return err
	}


	// Boot fuchsia.
	var bootMode int
	if cmd.netboot {
		bootMode = botanist.ModeNetboot
	} else {
		bootMode = botanist.ModePave
	}
	if err = botanist.Boot(ctx, addr, bootMode, imgs, cmdlineArgs, nil); err != nil {
		return err
	}

	// Handle host commands
	// TODO(IN-831): Remove when host-target-interaction infra is ready
	if cmd.hostCmd != "" {
		return cmd.runHostCmd(ctx)
	}

	logger.Debugf(ctx, "waiting for %q\n", cmd.summaryFilename)

	// Poll for summary.json; this relies on runtest being executed using
	// autorun and it eventually producing the summary.json file.
	t := tftp.NewClient()
	tftpAddr := &net.UDPAddr{
		IP:   addr.IP,
		Port: tftp.ClientPort,
		Zone: addr.Zone,
	}
	var buffer bytes.Buffer
	var writer io.WriterTo
	err = retry.Retry(ctx, retry.NewConstantBackoff(cmd.filePollInterval), func() error {
		writer, err = t.Receive(tftpAddr, path.Join(cmd.testResultsDir, cmd.summaryFilename))
		return err
	}, nil)
	if err != nil {
		return fmt.Errorf("timed out waiting for tests to complete: %v", err)
	}

	logger.Debugf(ctx, "reading %q\n", cmd.summaryFilename)

	if _, err := writer.WriteTo(&buffer); err != nil {
		return fmt.Errorf("failed to receive summary file: %v\n", err)
	}

	// Parse and save the summary.json file.
	var result runtests.TestSummary
	if err := json.Unmarshal(buffer.Bytes(), &result); err != nil {
		return fmt.Errorf("cannot unmarshall test results: %v\n", err)
	}

	outFile, err := cmd.createTarFile()
	if err != nil {
		return err
	}

	tw := tar.NewWriter(outFile)
	defer tw.Close()

	if err = botanist.ArchiveBuffer(tw, buffer.Bytes(), cmd.summaryFilename); err != nil {
		return err
	}

	logger.Debugf(ctx, "copying test output\n")

	// Tar in a subroutine while busy-printing so that we do not hit an i/o timeout when
	// dealing with large files.
	c := make(chan error)
	go func() {
		// Copy test output from the node.
		for _, output := range result.Outputs {
			remote := filepath.Join(cmd.testResultsDir, output)
			if err = botanist.FetchAndArchiveFile(t, tftpAddr, tw, remote, output); err != nil {
				c <- err
				return
			}
		}
		for _, test := range result.Tests {
			remote := filepath.Join(cmd.testResultsDir, test.OutputFile)
			if err = botanist.FetchAndArchiveFile(t, tftpAddr, tw, remote, test.OutputFile); err != nil {
				c <- err
				return
			}
			// Copy data sinks if any are present.
			for _, sinks := range test.DataSinks {
				for _, sink := range sinks {
					remote := filepath.Join(cmd.testResultsDir, sink.File)
					if err = botanist.FetchAndArchiveFile(t, tftpAddr, tw, remote, sink.File); err != nil {
						c <- err
						return
					}
				}
			}
		}
		c <- nil
	}()

	logger.Debugf(ctx, "tarring test output...\n")
	ticker := time.NewTicker(5 * time.Second)
	for {
		select {
		case err := <-c:
			ticker.Stop()
			return err
		case <-ticker.C:
			logger.Debugf(ctx, "tarring test output...\n")
		}
	}
}

func (cmd *ZedbootCommand) execute(ctx context.Context, cmdlineArgs []string) error {
	var properties botanist.DeviceProperties
	if err := botanist.LoadDeviceProperties(cmd.propertiesFile, &properties); err != nil {
		return fmt.Errorf("failed to open device properties file \"%v\"", cmd.propertiesFile)
	}

	if properties.PDU != nil {
		defer func() {
			logger.Debugf(ctx, "rebooting the node %q\n", properties.Nodename)

			if err := pdu.RebootDevice(properties.PDU); err != nil {
				logger.Fatalf(ctx, "failed to reboot the device: %v\n", err)
			}
		}()
	}

	ctx, cancel := context.WithCancel(ctx)

	// Handle SIGTERM and make sure we send a reboot to the device.
	signals := make(chan os.Signal, 1)
	signal.Notify(signals, syscall.SIGTERM)

	imgs, err := build.LoadImages(cmd.imageManifests...)
	if err != nil {
		return err
	}
	errs := make(chan error)
	go func() {
		if cmd.fastboot != "" {
			zirconR := imgs.Get("zircon-r")
			if zirconR == nil {
				errs <- fmt.Errorf("zircon-r not provided")
				return
			}
			// If it can't find any fastboot device, the fastboot
			// tool will hang waiting, so we add a timeout.
			// All fastboot operations take less than a second on
			// a developer workstation, so two minutes to flash and
			// continue is very generous.
			ctx, cancel := context.WithTimeout(ctx, 2*time.Minute)
			defer cancel()
			logger.Debugf(ctx, "flashing to zedboot with fastboot\n")
			if err := botanist.FastbootToZedboot(ctx, cmd.fastboot, zirconR.Path); err != nil {
				errs <- err
				return
			}
		}
		errs <- cmd.runTests(ctx, imgs, properties.Nodename, cmdlineArgs)
	}()

	select {
	case err := <-errs:
		return err
	case <-signals:
		cancel()
	}

	return nil
}

func (cmd *ZedbootCommand) Execute(ctx context.Context, f *flag.FlagSet, _ ...interface{}) subcommands.ExitStatus {
	propertiesFlag := f.Lookup("properties")
	logger.Debugf(ctx, "properties flag: %v\n", propertiesFlag.Value)

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
