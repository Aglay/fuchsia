// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
package main

import (
	"context"
	"flag"
	"fmt"
	"io/ioutil"
	"log"
	"net"
	"os"
	"os/exec"
	"os/signal"
	"syscall"
	"time"

	"fuchsia.googlesource.com/tools/botanist"
	"fuchsia.googlesource.com/tools/build"
	"fuchsia.googlesource.com/tools/netboot"
	"fuchsia.googlesource.com/tools/pdu"
	"fuchsia.googlesource.com/tools/retry"
	"github.com/google/subcommands"
	"golang.org/x/crypto/ssh"
)

// RunCommand is a Command implementation for booting a device and running a
// given command locally.
type RunCommand struct {
	// DeviceFile is the path to a file of device properties.
	deviceFile string

	// ImageManifests is a list of paths to image manifests (e.g., images.json)
	imageManifests botanist.StringsFlag

	// Netboot tells botanist to netboot (and not to pave).
	netboot bool

	// Fastboot is a path to the fastboot tool. If set, botanist will flash
	// the device into zedboot.
	fastboot string

	// ZirconArgs are kernel command-line arguments to pass on boot.
	zirconArgs botanist.StringsFlag

	// Timeout is the duration allowed for the command to finish execution.
	timeout time.Duration

	// CmdOutput is the file to which the command's stdout will be redirected.
	cmdOutput string

	// sshKey is the path to a private SSH user key.
	sshKey string
}

func (*RunCommand) Name() string {
	return "run"
}

func (*RunCommand) Usage() string {
	return `
botanist run [flags...] [command...]

flags:
`
}

func (*RunCommand) Synopsis() string {
	return "boots a device and runs a local command"
}

func (r *RunCommand) SetFlags(f *flag.FlagSet) {
	f.StringVar(&r.deviceFile, "device", "/etc/botanist/config.json", "path to file of device properties")
	f.Var(&r.imageManifests, "images", "paths to image manifests")
	f.BoolVar(&r.netboot, "netboot", false, "if set, botanist will not pave; but will netboot instead")
	f.StringVar(&r.fastboot, "fastboot", "", "path to the fastboot tool; if set, the device will be flashed into Zedboot. A zircon-r must be supplied via -images")
	f.Var(&r.zirconArgs, "zircon-args", "kernel command-line arguments")
	f.DurationVar(&r.timeout, "timeout", 10*time.Minute, "duration allowed for the command to finish execution.")
	f.StringVar(&r.cmdOutput, "output", "", "file to redirect the command's stdout into")
	f.StringVar(&r.sshKey, "ssh", "", "file containing a private SSH user key; if not provided, a private key will be generated.")
}

func (r *RunCommand) runCmd(ctx context.Context, imgs build.Images, nodename string, args []string, privKey []byte) error {
	// Find the node address UDP address.
	n := netboot.NewClient(time.Second)
	var addr *net.UDPAddr
	var err error

	// Set up log listener and dump kernel output to stdout.
	l, err := netboot.NewLogListener(nodename)
	if err != nil {
		return fmt.Errorf("cannot listen: %v\n", err)
	}
	go func() {
		defer l.Close()
		log.Printf("starting log listener\n")
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

	// We need to retry here because botanist might try to discover before
	// zedboot is fully ready, so the packet that's sent out doesn't result
	// in any reply. We don't need to wait between calls because Discover
	// already has a 1 minute timeout for reading a UDP packet from zedboot.
	err = retry.Retry(ctx, retry.WithMaxRetries(retry.NewConstantBackoff(time.Second), 60), func() error {
		addr, err = n.Discover(nodename, false)
		return err
	}, nil)
	if err != nil {
		return fmt.Errorf("cannot find node \"%s\": %v", nodename, err)
	}

	signer, err := ssh.ParsePrivateKey(privKey)
	if err != nil {
		return err
	}
	authorizedKey := ssh.MarshalAuthorizedKey(signer.PublicKey())

	// Boot fuchsia.
	var bootMode int
	if r.netboot {
		bootMode = botanist.ModeNetboot
	} else {
		bootMode = botanist.ModePave
	}
	if err = botanist.Boot(ctx, addr, bootMode, imgs, r.zirconArgs, authorizedKey); err != nil {
		return err
	}

	// Set device information in the environment for access within subprocesses.
	devCtx := botanist.DeviceContext{
		Nodename: nodename,
		SSHKey:   string(privKey),
	}
	if err = devCtx.Register(); err != nil {
		return fmt.Errorf("failed to set the device context: %v", err)
	}
	defer devCtx.Unregister()

	// Run command.
	// The subcommand is put in its own process group so that no subprocesses it spins up
	// are orphaned on cancelation.
	ctx, cancel := context.WithTimeout(ctx, r.timeout)
	defer cancel()
	cmd := exec.Cmd{
		Path:        args[0],
		Args:        args,
		Env:         append(os.Environ(), devCtx.EnvironEntry()),
		SysProcAttr: &syscall.SysProcAttr{Setpgid: true},
	}

	if r.cmdOutput != "" {
		f, err := os.Create(r.cmdOutput)
		if err != nil {
			return err
		}
		defer f.Close()
		cmd.Stdout = f
	}

	if err := cmd.Start(); err != nil {
		return err
	}
	done := make(chan error)
	go func() {
		done <- cmd.Wait()
	}()

	select {
	case err := <-done:
		return err
	case <-ctx.Done():
		syscall.Kill(-cmd.Process.Pid, syscall.SIGKILL)
	}
	return fmt.Errorf("command timed out after %v", r.timeout)
}

func (r *RunCommand) execute(ctx context.Context, args []string) error {
	imgs, err := build.LoadImages(r.imageManifests...)
	if err != nil {
		return fmt.Errorf("failed to load images: %v", err)
	}

	var privKey []byte
	if r.sshKey == "" {
		privKey, err = botanist.GeneratePrivateKey()
		if err != nil {
			return err
		}
	} else {
		privKey, err = ioutil.ReadFile(r.sshKey)
		if err != nil {
			return err
		}
	}

	var properties botanist.DeviceProperties
	if err := botanist.LoadDeviceProperties(r.deviceFile, &properties); err != nil {
		return fmt.Errorf("failed to open device properties file \"%v\": %v", r.deviceFile, err)
	}

	if properties.PDU != nil {
		defer func() {
			log.Printf("rebooting the node \"%s\"\n", properties.Nodename)

			if err := pdu.RebootDevice(properties.PDU); err != nil {
				log.Fatalf("failed to reboot %s: %v", properties.Nodename, err)
			}
		}()
	}

	ctx, cancel := context.WithCancel(ctx)

	// Handle SIGTERM and make sure we send a reboot to the device.
	signals := make(chan os.Signal, 1)
	signal.Notify(signals, syscall.SIGTERM)

	errs := make(chan error)
	go func() {
		if r.fastboot != "" {
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
			log.Printf("flashing to zedboot with fastboot")
			if err := botanist.FastbootToZedboot(ctx, r.fastboot, zirconR.Path); err != nil {
				errs <- err
				return
			}
		}
		errs <- r.runCmd(ctx, imgs, properties.Nodename, args, privKey)
	}()

	select {
	case err := <-errs:
		return err
	case <-signals:
		cancel()
	}

	return nil
}

func (r *RunCommand) Execute(ctx context.Context, f *flag.FlagSet, _ ...interface{}) subcommands.ExitStatus {
	args := f.Args()
	if len(args) == 0 {
		return subcommands.ExitUsageError
	}
	if err := r.execute(ctx, args); err != nil {
		log.Print(err)
		return subcommands.ExitFailure
	}
	return subcommands.ExitSuccess
}
