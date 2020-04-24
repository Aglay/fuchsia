// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package paver

import (
	"context"
	"io/ioutil"
	"log"
	"os"
	"os/exec"

	"golang.org/x/crypto/ssh"
)

type BuildPaver struct {
	paveZedbootScript string
	paveScript        string
	publicKey         ssh.PublicKey
}

type Paver interface {
	Pave(ctx context.Context, deviceName string) error
}

// NewPaver constructs a new paver, where script is the script to the "pave.sh"
// script, and `publicKey` is the public key baked into the device as an
// authorized key.
func NewBuildPaver(paveZedbootScript, paveScript string, publicKey ssh.PublicKey) *BuildPaver {
	return &BuildPaver{paveZedbootScript: paveZedbootScript, paveScript: paveScript, publicKey: publicKey}
}

// Pave runs a paver service for one pave. If `deviceName` is not empty, the
// pave will only be applied to the specified device.
func (p *BuildPaver) Pave(ctx context.Context, deviceName string) error {
	// Write out the public key's authorized keys.
	authorizedKeys, err := ioutil.TempFile("", "")
	if err != nil {
		return err
	}
	defer os.Remove(authorizedKeys.Name())

	if _, err := authorizedKeys.Write(ssh.MarshalAuthorizedKey(p.publicKey)); err != nil {
		return err
	}

	if err := authorizedKeys.Close(); err != nil {
		return err
	}

	// Run pave-zedboot.sh to bootstrap the new bootloader and zedboot.
	if err := p.runPave(ctx, deviceName, p.paveZedbootScript, "--allow-zedboot-version-mismatch"); err != nil {
		return err
	}

	// Run pave.sh to install Fuchsia.
	return p.runPave(ctx, deviceName, p.paveScript, "--fail-fast-if-version-mismatch", "--authorized-keys", authorizedKeys.Name())
}

func (p *BuildPaver) runPave(ctx context.Context, deviceName string, script string, args ...string) error {
	log.Printf("paving device %q with %s", deviceName, script)
	path, err := exec.LookPath(script)
	if err != nil {
		return err
	}

	args = append(args, "-1")
	if deviceName != "" {
		args = append(args, "-n", deviceName)
	}
	log.Printf("running: %s %q", path, args)
	cmd := exec.CommandContext(ctx, path, args...)
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	return cmd.Run()
}
