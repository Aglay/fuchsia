// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package qemu

import (
	"archive/tar"
	"bufio"
	"compress/gzip"
	"fmt"
	"io"
	"io/ioutil"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"
)

// Untar untars a tar.gz file into a directory.
func untar(dst string, src string) error {
	f, err := os.Open(src)
	if err != nil {
		return err
	}
	defer f.Close()

	gz, err := gzip.NewReader(f)
	if err != nil {
		return err
	}
	defer gz.Close()

	tr := tar.NewReader(gz)

	for {
		header, err := tr.Next()
		if err == io.EOF {
			return nil
		} else if err != nil {
			return err
		}

		path := filepath.Join(dst, header.Name)
		info := header.FileInfo()
		if info.IsDir() {
			if err := os.MkdirAll(path, info.Mode()); err != nil {
				return err
			}
		} else {
			if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
				return err
			}

			f, err := os.OpenFile(path, os.O_CREATE|os.O_EXCL|os.O_WRONLY, info.Mode())
			if err != nil {
				return err
			}

			if _, err := io.Copy(f, tr); err != nil {
				f.Close()
				return err
			}

			f.Close()
		}
	}
}

// Distribution is a collection of QEMU-related artifacts.
type Distribution struct {
	exPath       string
	unpackedPath string
}

type Arch int

const (
	X64 Arch = iota
	Arm64
)

// Params describes how to run a QEMU instance.
type Params struct {
	Arch          Arch
	ZBI           string
	AppendCmdline string
	Networking    bool
}

type Instance struct {
	cmd    *exec.Cmd
	stdin  *bufio.Writer
	stdout *bufio.Reader
	stderr *bufio.Reader
}

// Unpack unpacks the QEMU distribution.
func Unpack() (*Distribution, error) {
	ex, err := os.Executable()
	if err != nil {
		return nil, err
	}
	exPath := filepath.Dir(ex)
	archivePath := filepath.Join(exPath, "test_data/qemu/qemu.tar.gz")

	unpackedPath, err := ioutil.TempDir("", "qemu-distro")
	if err != nil {
		return nil, err
	}

	err = untar(unpackedPath, archivePath)
	if err != nil {
		os.RemoveAll(unpackedPath)
		return nil, err
	}

	return &Distribution{exPath: exPath, unpackedPath: unpackedPath}, nil
}

// Delete removes the QEMU-related artifacts.
func (d *Distribution) Delete() {
	os.RemoveAll(d.unpackedPath)
}

func (d *Distribution) systemPath(arch Arch) string {
	switch arch {
	case X64:
		return filepath.Join(d.unpackedPath, "bin/qemu-system-x86_64")
	case Arm64:
		return filepath.Join(d.unpackedPath, "bin/qemu-system-aarch64")
	}
	return ""
}

func (d *Distribution) kernelPath(arch Arch) string {
	switch arch {
	case X64:
		return filepath.Join(d.exPath, "test_data/qemu/multiboot.bin")
	case Arm64:
		return filepath.Join(d.exPath, "test_data/qemu/qemu-boot-shim.bin")
	}
	return ""
}

// TargetCPU returs the target CPU used by the build that produced this library.
func (d *Distribution) TargetCPU() (Arch, error) {
	path := filepath.Join(d.exPath, "test_data/qemu/target_cpu.txt")
	bytes, err := ioutil.ReadFile(path)
	if err != nil {
		return X64, err
	}
	name := string(bytes)
	switch name {
	case "x64":
		return X64, nil
	case "arm64":
		return Arm64, nil
	}
	return X64, fmt.Errorf("unknown target CPU: %s", name)
}

func (d *Distribution) appendCommonQemuArgs(params Params, args []string) []string {
	args = append(args, "-kernel", d.kernelPath(params.Arch))
	args = append(args, "-m", "2048", "-nographic", "-smp", "4,threads=2",
		"-machine", "q35", "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
		"-cpu", "Haswell,+smap,-check,-fsgsbase")
	if params.Networking {
		args = append(args, "-nic", "tap,ifname=qemu,script=no,downscript=no")
	} else {
		args = append(args, "-net", "none")
	}
	return args
}

func getCommonKernelCmdline(params Params) string {
	cmdline := "kernel.serial=legacy kernel.entropy-mixin=1420bb81dc0396b37cc2d0aa31bb2785dadaf9473d0780ecee1751afb5867564 kernel.halt-on-panic=true"
	if params.AppendCmdline != "" {
		cmdline += " "
		cmdline += params.AppendCmdline
	}
	return cmdline
}

// Create creates an instance of QEMU with the given parameters.
func (d *Distribution) Create(params Params) *Instance {
	path := d.systemPath(params.Arch)
	args := []string{}
	if params.ZBI == "" {
		panic("ZBI must be specified")
	}
	args = append(args, "-initrd", params.ZBI)
	args = d.appendCommonQemuArgs(params, args)
	args = append(args, "-append", getCommonKernelCmdline(params))
	return &Instance{
		cmd: exec.Command(path, args...),
	}
}

// Creates and runs an instance of QEMU that runs a single command and results
// the log that results from doing so.
// and `minfs` to be included in the BUILD.gn file (see disable_syscall_test's
// BUILD file.)
func (d *Distribution) RunNonInteractive(
	toRun string,
	hostPathMinfsBinary string,
	hostPathZbiBinary string,
	params Params) (string, string, error) {
	// This mode is non-interactive and is intended specifically to test the case
	// where the serial port has been disabled. The following modifications are
	// made to the QEMU invocation compared with Create()/Start():
	// - amalgamate the given ZBI into a larger one that includes an additional
	//   entry of a script which includes commands to run.
	// - that script mounts a disk created on the host in /tmp, and runs the
	//   given command with output redirected to a file also on the /tmp disk
	// - the script triggers shutdown of the machine
	// - after qemu shutdown, the log file is extracted and returned.
	//
	// In order to achive this, here we need to create the host minfs
	// filesystem, write the commands to run, build the augmented .zbi to
	// be used to boot. We then use Start() and wait for shutdown.
	// Finally, extract and return the log from the minfs disk.

	// Make the temp files we need.
	tmpFsFile, err := ioutil.TempFile(os.TempDir(), "*.fs")
	if err != nil {
		return "", "", err
	}
	defer os.Remove(tmpFsFile.Name())

	tmpRuncmds, err := ioutil.TempFile(os.TempDir(), "runcmds_*")
	if err != nil {
		return "", "", err
	}
	defer os.Remove(tmpRuncmds.Name())

	tmpZbi, err := ioutil.TempFile(os.TempDir(), "*.zbi")
	if err != nil {
		return "", "", err
	}
	defer os.Remove(tmpZbi.Name())

	tmpLog, err := ioutil.TempFile(os.TempDir(), "log.*.txt")
	if err != nil {
		return "", "", err
	}
	defer os.Remove(tmpLog.Name())

	tmpErr, err := ioutil.TempFile(os.TempDir(), "err.*.txt")
	if err != nil {
		return "", "", err
	}
	defer os.Remove(tmpErr.Name())

	// Write runcmds that mounts the results disk, runs the requested command, and
	// shuts down.
	tmpRuncmds.WriteString(`mkdir /tmp/testdata-fs
waitfor class=block topo=/dev/sys/pci/00:06.0/virtio-block/block timeout=60000
mount /dev/sys/pci/00:06.0/virtio-block/block /tmp/testdata-fs
`)
	tmpRuncmds.WriteString(toRun + " 2>/tmp/testdata-fs/err.txt >/tmp/testdata-fs/log.txt\n")
	tmpRuncmds.WriteString(`umount /tmp/testdata-fs
dm poweroff
`)

	// Make a minfs filesystem to mount in the target.
	cmd := exec.Command(hostPathMinfsBinary, tmpFsFile.Name()+"@100M", "mkfs")
	err = cmd.Run()
	if err != nil {
		return "", "", err
	}

	// Create the new initrd that references the runcmds file.
	cmd = exec.Command(
		hostPathZbiBinary, "-o", tmpZbi.Name(),
		params.ZBI,
		"-e", "runcmds="+tmpRuncmds.Name())
	err = cmd.Run()
	if err != nil {
		return "", "", err
	}

	// Build up the qemu command line from common arguments and the extra goop to
	// add the temporary disk at 00:06.0. This follows how infra runs qemu with an
	// extra disk via botanist.
	path := d.systemPath(params.Arch)
	args := []string{}
	args = append(args, "-initrd", tmpZbi.Name())
	args = d.appendCommonQemuArgs(params, args)
	args = append(args, "-object", "iothread,id=resultiothread")
	args = append(args, "-drive",
		"id=resultdisk,file="+tmpFsFile.Name()+",format=raw,if=none,cache=unsafe,aio=threads")
	args = append(args, "-device", "virtio-blk-pci,drive=resultdisk,iothread=resultiothread,addr=6.0")

	cmdline := getCommonKernelCmdline(params)
	cmdline += " zircon.autorun.boot=/boot/bin/sh+/boot/runcmds"
	args = append(args, "-append", cmdline)

	i := &Instance{cmd: exec.Command(path, args...)}
	err = i.Start()
	if err != nil {
		return "", "", err
	}
	defer i.Kill()

	err = i.cmd.Wait()
	if err != nil {
		return "", "", err
	}

	os.Remove(tmpLog.Name()) // `minfs` will refuse to overwrite a local file, so delete first.
	cmd = exec.Command(hostPathMinfsBinary, tmpFsFile.Name(), "cp", "::/log.txt", tmpLog.Name())
	err = cmd.Run()
	if err != nil {
		return "", "", err
	}

	os.Remove(tmpErr.Name()) // `minfs` will refuse to overwrite a local file, so delete first.
	cmd = exec.Command(hostPathMinfsBinary, tmpFsFile.Name(), "cp", "::/err.txt", tmpErr.Name())
	err = cmd.Run()
	if err != nil {
		return "", "", err
	}

	retLog, err := ioutil.ReadFile(tmpLog.Name())
	if err != nil {
		return "", "", err
	}
	retErr, err := ioutil.ReadFile(tmpErr.Name())
	if err != nil {
		return "", "", err
	}
	return string(retLog), string(retErr), nil
}

// Start the QEMU instance.
func (i *Instance) Start() error {
	stdin, err := i.cmd.StdinPipe()
	if err != nil {
		return err
	}
	stdout, err := i.cmd.StdoutPipe()
	if err != nil {
		return err
	}
	stderr, err := i.cmd.StderrPipe()
	if err != nil {
		return err
	}
	i.stdin = bufio.NewWriter(stdin)
	i.stdout = bufio.NewReader(stdout)
	i.stderr = bufio.NewReader(stderr)

	startErr := i.cmd.Start()

	// Look for very early log message to validate that qemu likely started
	// correctly. Loop for a while to give qemu a chance to boot.
	for j := 0; j < 100; j++ {
		if i.CheckForLogMessage("SeaBIOS") == nil {
			break
		}
		time.Sleep(100 * time.Millisecond)
	}

	return startErr
}

// Kill terminates the QEMU instance.
func (i *Instance) Kill() error {
	return i.cmd.Process.Kill()
}

// RunCommand runs the given command in the serial console for the QEMU instance.
func (i *Instance) RunCommand(cmd string) {
	_, err := i.stdin.WriteString(fmt.Sprintf("%s\n", cmd))
	if err != nil {
		panic(err)
	}
	err = i.stdin.Flush()
	if err != nil {
		panic(err)
	}
}

// WaitForLogMessage reads log messages from the QEMU instance until it reads a
// message that contains the given string. panic()s on error (and in particular
// if the string is not seen until EOF).
func (i *Instance) WaitForLogMessage(msg string) {
	err := i.CheckForLogMessage(msg)
	if err != nil {
		panic(err)
	}
}

// WaitForLogMessageAssertNotSeen is the same as WaitForLogMessage() but with
// the addition that it will panic if |notSeen| is contained in a retrieved
// message.
func (i *Instance) WaitForLogMessageAssertNotSeen(msg string, notSeen string) {
	for {
		line, err := i.stdout.ReadString('\n')
		if err != nil {
			panic(err)
		}
		if strings.Contains(line, msg) {
			return
		}
		if strings.Contains(line, notSeen) {
			panic(notSeen + " was in output")
		}
	}
}

// Reset display: ESC c
// Reset screen mode: ESC [ ? 7 l
// Move cursor home: ESC [ 2 J
// All text attributes off: ESC [ 0 m
const qemuClearPrefix = "\x1b\x63\x1b\x5b\x3f\x37\x6c\x1b\x5b\x32\x4a\x1b\x5b\x30\x6d"

// Reads all messages from stdout of QEMU, and tests if msg appears. Returns
// error if any. Prefer WaitForLogMessage() unless you're certain this is the
// one you want.
func (i *Instance) CheckForLogMessage(msg string) error {
	for {
		line, err := i.stdout.ReadString('\n')
		if err != nil {
			panic(err)
		}

		// Drop the QEMU clearing preamble as it makes it difficult to see output
		// when there's multiple qemu runs in a single binary.
		toPrint := line
		if strings.HasPrefix(toPrint, qemuClearPrefix) {
			toPrint = toPrint[len(qemuClearPrefix):]
		}
		fmt.Print(toPrint)

		if strings.Contains(line, msg) {
			return nil
		}
	}
}
