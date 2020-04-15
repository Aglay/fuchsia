// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package device

import (
	"bytes"
	"context"
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"io"
	"io/ioutil"
	"log"
	"net"
	"os"
	"strings"
	"sync"
	"time"

	"fuchsia.googlesource.com/host_target_testing/artifacts"
	"fuchsia.googlesource.com/host_target_testing/packages"
	"fuchsia.googlesource.com/host_target_testing/sl4f"
	"go.fuchsia.dev/fuchsia/tools/net/sshutil"

	"golang.org/x/crypto/ssh"
)

const rebootCheckPath = "/tmp/ota_test_should_reboot"

type RecoveryMode int

const (
	RebootToRecovery RecoveryMode = iota
	OTAToRecovery
)

// Client manages the connection to the device.
type Client struct {
	Name           string
	deviceHostname string
	addr           net.Addr
	sshConfig      *ssh.ClientConfig

	// This mutex protects the following fields.
	mu        sync.Mutex
	sshClient *sshutil.Client
}

// NewClient creates a new Client.
func NewClient(ctx context.Context, deviceHostname string, name string, privateKey ssh.Signer) (*Client, error) {
	sshConfig, err := newSSHConfig(privateKey)
	if err != nil {
		return nil, err
	}

	addr, err := net.ResolveTCPAddr("tcp", net.JoinHostPort(deviceHostname, "22"))
	if err != nil {
		return nil, err
	}
	sshClient, err := sshutil.NewClient(ctx, addr, sshConfig)
	if err != nil {
		return nil, err
	}

	return &Client{
		Name:           name,
		deviceHostname: deviceHostname,
		addr:           addr,
		sshConfig:      sshConfig,
		sshClient:      sshClient,
	}, nil
}

// Construct a new `ssh.ClientConfig` for a given key file, or return an error if
// the key is invalid.
func newSSHConfig(privateKey ssh.Signer) (*ssh.ClientConfig, error) {
	config := &ssh.ClientConfig{
		User: "fuchsia",
		Auth: []ssh.AuthMethod{
			ssh.PublicKeys(privateKey),
		},
		HostKeyCallback: ssh.InsecureIgnoreHostKey(),
		Timeout:         30 * time.Second,
	}

	return config, nil
}

// Close the Client connection
func (c *Client) Close() {
	c.mu.Lock()
	defer c.mu.Unlock()

	c.sshClient.Close()
}

func (c *Client) Reconnect(ctx context.Context) error {
	c.mu.Lock()
	defer c.mu.Unlock()

	c.sshClient.Close()

	sshClient, err := sshutil.NewClient(ctx, c.addr, c.sshConfig)
	if err != nil {
		return err
	}
	c.sshClient = sshClient

	return nil
}

// Run a command to completion on the remote device and write STDOUT and STDERR
// to the passed in io.Writers.
func (c *Client) Run(ctx context.Context, command []string, stdout io.Writer, stderr io.Writer) error {
	c.mu.Lock()
	sshClient := c.sshClient
	c.mu.Unlock()

	return sshClient.Run(ctx, command, stdout, stderr)
}

// RegisterDisconnectListener adds a waiter that gets notified when the ssh and
// shell is disconnected.
func (c *Client) RegisterDisconnectListener(ch chan struct{}) {
	c.mu.Lock()
	sshClient := c.sshClient
	c.mu.Unlock()

	sshClient.RegisterDisconnectListener(ch)
}

func (c *Client) GetSSHConnection(ctx context.Context) (string, error) {
	var stdout bytes.Buffer
	var stderr bytes.Buffer
	cmd := []string{"PATH=''", "echo", "$SSH_CONNECTION"}
	if err := c.Run(ctx, cmd, &stdout, &stderr); err != nil {
		return "", fmt.Errorf("failed to read SSH_CONNECTION: %s: %s", err, string(stderr.Bytes()))
	}
	return strings.Split(string(stdout.Bytes()), " ")[0], nil
}

func (c *Client) GetSystemImageMerkle(ctx context.Context) (string, error) {
	const systemImageMeta = "/system/meta"
	merkle, err := c.ReadRemotePath(ctx, systemImageMeta)
	if err != nil {
		return "", err
	}

	return strings.TrimSpace(string(merkle)), nil
}

// Reboot asks the device to reboot. It waits until the device reconnects
// before returning.
func (c *Client) Reboot(ctx context.Context) error {
	log.Printf("rebooting")

	return c.ExpectReboot(ctx, func() error {
		// Run the reboot in the background, which gives us a chance to
		// observe us successfully executing the reboot command.
		cmd := []string{"dm", "reboot", "&", "exit", "0"}
		if err := c.Run(ctx, cmd, os.Stdout, os.Stderr); err != nil {
			// If the device rebooted before ssh was able to tell
			// us the command ran, it will tell us the session
			// exited without passing along an exit code. So,
			// ignore that specific error.
			if _, ok := err.(*ssh.ExitMissingError); ok {
				log.Printf("ssh disconnected before returning a status")
			} else {
				return fmt.Errorf("failed to reboot: %s", err)
			}
		}

		return nil
	})
}

// RebootToRecovery asks the device to reboot into the recovery partition. It
// waits until the device disconnects before returning.
func (c *Client) RebootToRecovery(ctx context.Context) error {
	log.Printf("Rebooting to recovery")

	return c.ExpectDisconnect(ctx, func() error {
		// Run the reboot in the background, which gives us a chance to
		// observe us successfully executing the reboot command.
		cmd := []string{"dm", "reboot-recovery", "&", "exit", "0"}
		if err := c.Run(ctx, cmd, os.Stdout, os.Stderr); err != nil {
			// If the device rebooted before ssh was able to tell
			// us the command ran, it will tell us the session
			// exited without passing along an exit code. So,
			// ignore that specific error.
			if _, ok := err.(*ssh.ExitMissingError); ok {
				log.Printf("ssh disconnected before returning a status")
			} else {
				return fmt.Errorf("failed to reboot into recovery: %w", err)
			}
		}

		return nil
	})
}

// OTAToRecovery asks the device to OTA to the
// fuchsia-pkg://fuchsia.com/update-to-recovery package. It waits until the
// device disconnects before returning.
func (c *Client) OTAToRecovery(ctx context.Context, repo *packages.Repository) error {
	log.Printf("OTAing to recovery")

	if err := c.DownloadOTA(ctx, repo, "fuchsia-pkg://fuchsia.com/update-to-zedboot/0"); err != nil {
		return fmt.Errorf("failed to download OTA: %w", err)
	}

	return c.ExpectDisconnect(ctx, func() error {
		cmd := []string{"dm", "reboot", "&", "exit", "0"}
		err := c.Run(ctx, cmd, os.Stdout, os.Stderr)

		if err != nil {
			// If the device rebooted before ssh was able to tell
			// us the command ran, it will tell us the session
			// exited without passing along an exit code. So,
			// ignore that specific error.
			if _, ok := err.(*ssh.ExitMissingError); ok {
				log.Printf("ssh disconnected before returning a status")
			} else {
				return fmt.Errorf("failed to OTA into recovery: %w", err)
			}
		}

		return nil
	})
}

func (c *Client) ReadBasePackages(ctx context.Context) (map[string]string, error) {
	b, err := c.ReadRemotePath(ctx, "/pkgfs/system/data/static_packages")
	if err != nil {
		return nil, err
	}

	pkgs := make(map[string]string)
	for _, line := range strings.Split(string(b), "\n") {
		if line == "" {
			break
		}
		parts := strings.SplitN(line, "=", 2)
		pkgs[parts[0]] = parts[1]
	}

	return pkgs, nil
}

// TriggerSystemOTA gets the device to perform a system update, ensuring it
// reboots as expected.
func (c *Client) TriggerSystemOTA(ctx context.Context, repo *packages.Repository) error {
	log.Printf("Triggering OTA")
	startTime := time.Now()

	basePackages, err := c.ReadBasePackages(ctx)
	if err != nil {
		return err
	}

	updateBinMerkle, ok := basePackages["update-bin/0"]
	if !ok {
		return fmt.Errorf("base packages doesn't include update-bin/0 package")
	}

	return c.ExpectReboot(ctx, func() error {
		server, err := c.ServePackageRepository(ctx, repo, "trigger-ota", true)
		if err != nil {
			return fmt.Errorf("error setting up server: %s", err)
		}
		defer server.Shutdown(ctx)

		// FIXME: running this out of /pkgfs/versions is unsound WRT using the correct loader service
		// Adding this as a short-term hack to unblock http://fxb/47213
		cmd := []string{
			fmt.Sprintf("/pkgfs/versions/%s/bin/update", updateBinMerkle),
			"check-now",
			"--monitor",
		}
		if err := c.Run(ctx, cmd, os.Stdout, os.Stderr); err != nil {
			// If the device rebooted before ssh was able to tell
			// us the command ran, it will tell us the session
			// exited without passing along an exit code. So,
			// ignore that specific error.
			if _, ok := err.(*ssh.ExitMissingError); !ok {
				return fmt.Errorf("failed to trigger OTA: %s", err)
			}
		}

		return nil
	})
	if err != nil {
		return err
	}

	log.Printf("OTA completed in %s", time.Now().Sub(startTime))

	return nil
}

func (c *Client) ExpectDisconnect(ctx context.Context, f func() error) error {
	ch := make(chan struct{})
	c.RegisterDisconnectListener(ch)

	if err := f(); err != nil {
		// It's okay if we leak the disconnect listener, it'll get
		// cleaned up next time the device disconnects.
		return err
	}

	// Wait until we get a signal that we have disconnected
	select {
	case <-ch:
	case <-ctx.Done():
		return fmt.Errorf("device did not disconnect: %s", ctx.Err())
	}

	log.Printf("device disconnected")

	return nil
}

// ExpectReboot prepares a device for a reboot, runs a closure `f` that should
// reboot the device, then finally verifies whether a reboot actually took
// place. It does this by writing a unique value to
// `/tmp/ota_test_should_reboot`, then executing the closure. After we
// reconnect, we check if `/tmp/ota_test_should_reboot` exists. If not, exit
// with `nil`. Otherwise, we failed to reboot, or some competing test is also
// trying to reboot the device. Either way, err out.
func (c *Client) ExpectReboot(ctx context.Context, f func() error) error {
	// Generate a unique value.
	b := make([]byte, 16)
	_, err := rand.Read(b)
	if err != nil {
		return fmt.Errorf("failed to generate a unique boot number: %w", err)
	}

	// Encode the id into hex so we can write it through the shell.
	bootID := hex.EncodeToString(b)

	// Write the value to the file. Err if the file already exists by setting the
	// noclobber setting.
	cmd := fmt.Sprintf(
		`(
			set -C &&
			PATH= echo "%s" > "%s"
        )`, bootID, rebootCheckPath)
	err = c.Run(ctx, strings.Fields(cmd), os.Stdout, os.Stderr)
	if err != nil {
		return fmt.Errorf("failed to write reboot check file: %w", err)
	}

	// As a sanity check, make sure the file actually exists and has the correct
	// value.
	b, err = c.ReadRemotePath(ctx, rebootCheckPath)
	if err != nil {
		return fmt.Errorf("failed to read reboot check file: %w", err)
	}
	actual := strings.TrimSpace(string(b))

	if actual != bootID {
		return fmt.Errorf("reboot check file has wrong value: expected %q, got %q", bootID, actual)
	}

	// We are finally ready to run the closure. Setup a disconnection listener,
	// then execute the closure.
	ch := make(chan struct{})
	c.RegisterDisconnectListener(ch)

	if err := f(); err != nil {
		// It's okay if we leak the disconnect listener, it'll get
		// cleaned up next time the device disconnects.
		return err
	}

	// Wait until we get a signal that we have disconnected
	select {
	case <-ch:
	case <-ctx.Done():
		return fmt.Errorf("device did not disconnect: %w", ctx.Err())
	}

	log.Printf("device disconnected, waiting for device to boot")

	if err := c.Reconnect(ctx); err != nil {
		return fmt.Errorf("failed to reconnect: %w", err)
	}

	// We reconnected to the device. Check that the reboot check file doesn't exist.
	exists, err := c.RemoteFileExists(ctx, rebootCheckPath)
	if err != nil {
		return fmt.Errorf(`failed to check if "%s" exists: %w`, err)
	}
	if exists {
		// The reboot file exists. This could have happened because either we
		// didn't reboot, or some other test is also trying to reboot the
		// device. We can distinguish the two by comparing the file contents
		// with the bootID we wrote earlier.
		b, err := c.ReadRemotePath(ctx, rebootCheckPath)
		if err != nil {
			return fmt.Errorf("failed to read reboot check file: %w", err)
		}
		actual := strings.TrimSpace(string(b))

		// If the contents match, then we failed to reboot.
		if actual == bootID {
			return fmt.Errorf("reboot check file exists after reboot, device did not reboot")
		}

		return fmt.Errorf(
			"reboot check file exists after reboot, and has unexpected value: expected %q, got %q",
			bootID,
			actual,
		)
	}

	log.Printf("device rebooted")

	return nil
}

// ValidateStaticPackages checks that all static packages have no missing blobs.
func (c *Client) ValidateStaticPackages(ctx context.Context) error {
	log.Printf("validating static packages")

	path := "/pkgfs/ctl/validation/missing"
	f, err := c.ReadRemotePath(ctx, path)
	if err != nil {
		return fmt.Errorf("error reading %q: %s", path, err)
	}

	merkles := strings.TrimSpace(string(f))
	if merkles != "" {
		return fmt.Errorf("static packages are missing the following blobs:\n%s", merkles)
	}

	log.Printf("all static package blobs are accounted for")
	return nil
}

// ReadRemotePath read a file off the remote device.
func (c *Client) ReadRemotePath(ctx context.Context, path string) ([]byte, error) {
	var stdout bytes.Buffer
	var stderr bytes.Buffer
	cmd := fmt.Sprintf(
		`(
		test -e "%s" &&
		while IFS='' read f; do
			echo "$f";
		done < "%s" &&
		if [ ! -z "$f" ];
			then echo "$f";
		fi
		)`, path, path)
	if err := c.Run(ctx, strings.Fields(cmd), &stdout, &stderr); err != nil {
		return nil, fmt.Errorf("failed to read %q: %s: %s", path, err, string(stderr.Bytes()))
	}

	return stdout.Bytes(), nil
}

// DeleteRemotePath deletes a file off the remote device.
func (c *Client) DeleteRemotePath(ctx context.Context, path string) error {
	var stderr bytes.Buffer
	cmd := []string{"PATH=''", "rm", path}
	if err := c.Run(ctx, cmd, os.Stdout, &stderr); err != nil {
		return fmt.Errorf("failed to delete %q: %s: %s", path, err, string(stderr.Bytes()))
	}

	return nil
}

// RemoteFileExists checks if a file exists on the remote device.
func (c *Client) RemoteFileExists(ctx context.Context, path string) (bool, error) {
	var stderr bytes.Buffer
	cmd := []string{"PATH=''", "test", "-e", path}

	if err := c.Run(ctx, cmd, ioutil.Discard, &stderr); err != nil {
		if e, ok := err.(*ssh.ExitError); ok {
			if e.ExitStatus() == 1 {
				return false, nil
			}
		}
		return false, fmt.Errorf("error reading %q: %s: %s", path, err, string(stderr.Bytes()))
	}

	return true, nil
}

// RegisterPackageRepository adds the repository as a repository inside the device.
func (c *Client) RegisterPackageRepository(ctx context.Context, repo *packages.Server, createRewriteRule bool) error {
	log.Printf("registering package repository: %s", repo.Dir)
	var subcmd string
	if createRewriteRule {
		subcmd = "add_src"
	} else {
		subcmd = "add_repo_cfg"
	}
	cmd := []string{"amberctl", subcmd, "-f", repo.URL, "-h", repo.Hash, "-verbose"}
	return c.Run(ctx, cmd, os.Stdout, os.Stderr)
}

func (c *Client) ServePackageRepository(
	ctx context.Context,
	repo *packages.Repository,
	name string,
	createRewriteRule bool,
) (*packages.Server, error) {
	// Make sure the device doesn't have any broken static packages.
	if err := c.ValidateStaticPackages(ctx); err != nil {
		return nil, err
	}

	// Tell the device to connect to our repository.
	localHostname, err := c.GetSSHConnection(ctx)
	if err != nil {
		return nil, err
	}

	// Serve the repository before the test begins.
	server, err := repo.Serve(ctx, localHostname, name)
	if err != nil {
		return nil, err
	}

	if err := c.RegisterPackageRepository(ctx, server, createRewriteRule); err != nil {
		server.Shutdown(ctx)
		return nil, err
	}

	return server, nil
}

func (c *Client) StartRpcSession(ctx context.Context, repo *packages.Repository) (*sl4f.Client, error) {
	log.Printf("connecting to sl4f")
	startTime := time.Now()

	c.mu.Lock()
	sshClient := c.sshClient
	c.mu.Unlock()

	// Ensure this client is running system_image or system_image_prime from repo.
	currentSystemImageMerkle, err := c.GetSystemImageMerkle(ctx)
	if err != nil {
		return nil, fmt.Errorf("failed to get system image merkle: %s", err)
	}
	if err := repo.VerifyMatchesAnyUpdateSystemImageMerkle(currentSystemImageMerkle); err != nil {
		return nil, fmt.Errorf("repo does not match system version: %s", err)
	}

	// Configure the target to use this repository as "fuchsia-pkg://host_target_testing_sl4f".
	repoName := "host_target_testing_sl4f"
	repoServer, err := c.ServePackageRepository(ctx, repo, repoName, false)
	if err != nil {
		return nil, fmt.Errorf("error serving repo to device: %s", err)
	}
	defer repoServer.Shutdown(ctx)

	rpcClient, err := sl4f.NewClient(ctx, sshClient, net.JoinHostPort(c.deviceHostname, "80"), repoName)
	if err != nil {
		return nil, fmt.Errorf("error creating sl4f client: %s", err)
	}

	log.Printf("connected to sl4f in %s", time.Now().Sub(startTime))

	return rpcClient, nil
}

func (c *Client) DownloadOTA(ctx context.Context, repo *packages.Repository, updatePackageUrl string) error {
	log.Printf("Downloading OTA")
	startTime := time.Now()

	server, err := c.ServePackageRepository(ctx, repo, "download-ota", true)
	if err != nil {
		return fmt.Errorf("error setting up server: %s", err)
	}
	defer server.Shutdown(ctx)

	// In order to manually trigger the system updater, we need the `run`
	// package. Since builds can be configured to not automatically install
	// packages, we need to explicitly resolve it.
	cmd := []string{"pkgctl", "resolve", "fuchsia-pkg://fuchsia.com/run/0"}
	if err := c.Run(ctx, cmd, os.Stdout, os.Stderr); err != nil {
		return fmt.Errorf("error resolving the run package: %v", err)
	}

	log.Printf("Downloading system OTA")

	cmd = []string{
		"run",
		"\"fuchsia-pkg://fuchsia.com/amber#meta/system_updater.cmx\"",
		"--initiator", "manual",
		// Go's boolean flag parsing requires that the argument name and value
		// be separated by "=" instead of by whitespace.
		"--reboot=false",
		"--update", fmt.Sprintf("%q", updatePackageUrl),
	}
	if err := c.Run(ctx, cmd, os.Stdout, os.Stderr); err != nil {
		return fmt.Errorf("failed to run system_updater.cmx: %s", err)
	}

	log.Printf("OTA successfully downloaded in %s", time.Now().Sub(startTime))

	return nil
}

// Pave paves the device to the specified build. It assumes the device is
// already in recovery, since there are multiple ways to get a device into
// recovery.
func (c *Client) Pave(ctx context.Context, build artifacts.Build, mode RecoveryMode) error {
	paver, err := build.GetPaver(ctx)
	if err != nil {
		return fmt.Errorf("failed to get paver to pave device: %w", err)
	}

	switch mode {
	case RebootToRecovery:
		if err := c.RebootToRecovery(ctx); err != nil {
			return fmt.Errorf("failed to reboot to recovery during paving: %w", err)
		}

	case OTAToRecovery:
		repo, err := build.GetPackageRepository(ctx)
		if err != nil {
			return fmt.Errorf("failed to get repo to OTA device to recovery: %w", err)
		}

		if err := c.OTAToRecovery(ctx, repo); err != nil {
			return fmt.Errorf("failed to reboot to recovery during paving: %w", err)
		}
	default:
		return fmt.Errorf("unknown recovery mode: %d", mode)
	}

	// Actually pave the device.
	if err = paver.Pave(ctx, c.Name); err != nil {
		return fmt.Errorf("device failed to pave: %w", err)
	}

	// Reconnect to the device.
	if err = c.Reconnect(ctx); err != nil {
		return fmt.Errorf("device failed to connect after pave: %w", err)
	}

	return nil
}
