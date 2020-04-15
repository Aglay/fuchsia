// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package reboot

import (
	"context"
	"flag"
	"fmt"
	"log"
	"os"
	"testing"

	"fuchsia.googlesource.com/host_target_testing/artifacts"
	"fuchsia.googlesource.com/host_target_testing/device"
	"fuchsia.googlesource.com/host_target_testing/sl4f"
	"fuchsia.googlesource.com/system_tests/pave"
)

var c *config

func TestMain(m *testing.M) {
	log.SetPrefix("reboot-test: ")
	log.SetFlags(log.Ldate | log.Ltime | log.LUTC | log.Lshortfile)

	var err error
	c, err = newConfig(flag.CommandLine)
	if err != nil {
		log.Fatalf("failed to create config: %s", err)
	}

	flag.Parse()

	if err = c.validate(); err != nil {
		log.Fatalf("config is invalid: %s", err)
	}

	os.Exit(m.Run())
}

func TestReboot(t *testing.T) {
	ctx := context.Background()

	outputDir, cleanup, err := c.archiveConfig.OutputDir()
	if err != nil {
		t.Fatal(err)
	}
	defer cleanup()

	device, err := c.deviceConfig.NewDeviceClient(ctx)
	if err != nil {
		t.Fatalf("failed to create ota test client: %s", err)
	}
	defer device.Close()

	build, err := c.getBuild(ctx, outputDir)
	if err != nil {
		log.Fatalf("failed to get downgrade build: %v", err)
	}

	if err := initializeDevice(ctx, device, build); err != nil {
		t.Fatalf("paving failed: %s", err)
	}

	testReboot(ctx, device, build)
}

func testReboot(ctx context.Context, device *device.Client, build artifacts.Build) {
	for i := 1; i <= c.cycleCount; i++ {
		log.Printf("Reboot Attempt %d", i)

		if err := doTestReboot(ctx, device, build); err != nil {
			log.Fatalf("OTA Cycle %d timed out: %s", i, err)
		}
	}
}

func doTestReboot(ctx context.Context, device *device.Client, build artifacts.Build) error {
	ctx, cancel := context.WithTimeout(ctx, c.cycleTimeout)
	defer cancel()

	repo, err := build.GetPackageRepository(ctx)
	if err != nil {
		return fmt.Errorf("unable to get repository: %w", err)
	}

	rpcClient, err := device.StartRpcSession(ctx, repo)
	if err != nil {
		return fmt.Errorf("unable to connect to sl4f: %s", err)
	}
	defer func() {
		if rpcClient != nil {
			rpcClient.Close()
		}
	}()

	// Install version N on the device if it is not already on that version.
	expectedSystemImageMerkle, err := repo.LookupUpdateSystemImageMerkle()
	if err != nil {
		return fmt.Errorf("error extracting expected system image merkle: %s", err)
	}

	expectedConfig, err := determineActiveConfig(ctx, rpcClient)
	if err != nil {
		return fmt.Errorf("error determining target config: %s", err)
	}

	if err := validateDevice(ctx, device, rpcClient, expectedSystemImageMerkle, expectedConfig); err != nil {
		return err
	}

	// Disconnect from sl4f since the OTA should reboot the device.
	rpcClient.Close()
	rpcClient = nil

	if err := device.Reboot(ctx); err != nil {
		return fmt.Errorf("error rebooting: %s", err)
	}

	rpcClient, err = device.StartRpcSession(ctx, repo)
	if err != nil {
		return fmt.Errorf("unable to connect to sl4f: %s", err)
	}

	return validateDevice(ctx, device, rpcClient, expectedSystemImageMerkle, expectedConfig)
}

func initializeDevice(
	ctx context.Context,
	device *device.Client,
	build artifacts.Build,
) error {
	log.Printf("Initializing device")

	ctx, cancel := context.WithTimeout(ctx, c.paveTimeout)
	defer cancel()

	repo, err := build.GetPackageRepository(ctx)
	if err != nil {
		return err
	}

	expectedSystemImageMerkle, err := repo.LookupUpdateSystemImageMerkle()
	if err != nil {
		return fmt.Errorf("error extracting expected system image merkle: %w", err)
	}

	// Only pave if the device is not running the expected version.
	upToDate, err := isDeviceUpToDate(ctx, device, expectedSystemImageMerkle)
	if err != nil {
		return fmt.Errorf("failed to check if up to date during initialization: %w", err)
	}
	if upToDate {
		log.Printf("device already up to date")
		return nil
	}

	if err := pave.PaveDevice(ctx, device, build, c.otaToRecovery); err != nil {
		return fmt.Errorf("failed to pave device during initialization: %w", err)
	}

	rpcClient, err := device.StartRpcSession(ctx, repo)
	if err != nil {
		return fmt.Errorf("unable to connect to sl4f: %w", err)
	}
	defer rpcClient.Close()

	// Check if we support ABR. If so, we always boot into A after a pave.
	expectedConfig, err := determineActiveConfig(ctx, rpcClient)
	if err != nil {
		return err
	}
	if expectedConfig != nil {
		config := sl4f.ConfigurationA
		expectedConfig = &config
	}

	if err := validateDevice(ctx, device, rpcClient, expectedSystemImageMerkle, expectedConfig); err != nil {
		return err
	}

	return nil
}

func isDeviceUpToDate(ctx context.Context, device *device.Client, expectedSystemImageMerkle string) (bool, error) {
	// Get the device's current /system/meta. Error out if it is the same
	// version we are about to OTA to.
	remoteSystemImageMerkle, err := device.GetSystemImageMerkle(ctx)
	if err != nil {
		return false, err
	}
	log.Printf("current system image merkle: %q", remoteSystemImageMerkle)
	log.Printf("upgrading to system image merkle: %q", expectedSystemImageMerkle)

	return expectedSystemImageMerkle == remoteSystemImageMerkle, nil
}

func determineActiveConfig(ctx context.Context, rpcClient *sl4f.Client) (*sl4f.Configuration, error) {
	activeConfig, err := rpcClient.PaverQueryActiveConfiguration(ctx)
	if err == sl4f.ErrNotSupported {
		log.Printf("device does not support querying the active configuration")
		return nil, nil
	} else if err != nil {
		return nil, err
	}

	log.Printf("device booted to slot %s", activeConfig)

	return &activeConfig, nil
}

func validateDevice(
	ctx context.Context,
	device *device.Client,
	rpcClient *sl4f.Client,
	expectedSystemImageMerkle string,
	expectedConfig *sl4f.Configuration,
) error {
	// At the this point the system should have been updated to the target
	// system version. Confirm the update by fetching the device's current
	// /system/meta, and making sure it is the correct version.
	upToDate, err := isDeviceUpToDate(ctx, device, expectedSystemImageMerkle)
	if err != nil {
		return err
	}
	if !upToDate {
		return fmt.Errorf("system version failed to update to %q", expectedSystemImageMerkle)
	}

	// Make sure the device doesn't have any broken static packages.
	if err := rpcClient.ValidateStaticPackages(ctx); err != nil {
		return err
	}

	// Ensure the device is booting from the expected boot slot
	activeConfig, err := determineActiveConfig(ctx, rpcClient)
	if err != nil {
		return fmt.Errorf("unable to determine active boot configuration: %s", err)
	}

	if expectedConfig != nil && activeConfig != nil && *activeConfig != *expectedConfig {
		return fmt.Errorf("expected device to boot from slot %s, got %s", *expectedConfig, *activeConfig)
	}

	return nil
}
