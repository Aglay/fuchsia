// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package upgrade

import (
	"context"
	"flag"
	"fmt"
	"log"
	"os"
	"testing"
	"time"

	"fuchsia.googlesource.com/host_target_testing/device"
	"fuchsia.googlesource.com/host_target_testing/packages"
	"fuchsia.googlesource.com/host_target_testing/sl4f"
)

var c *Config

func TestMain(m *testing.M) {
	log.SetPrefix("upgrade-test: ")
	log.SetFlags(log.Ldate | log.Ltime | log.LUTC | log.Lshortfile)

	var err error
	c, err = NewConfig(flag.CommandLine)
	if err != nil {
		log.Fatalf("failed to create config: %s", err)
	}

	flag.Parse()

	if err = c.Validate(); err != nil {
		log.Fatalf("config is invalid: %s", err)
	}

	os.Exit(m.Run())
}

func TestOTA(t *testing.T) {
	ctx := context.Background()

	device, err := c.deviceConfig.NewDeviceClient(ctx)
	if err != nil {
		log.Fatalf("failed to create ota test client: %s", err)
	}
	defer device.Close()

	// Creating a sl4f.Client requires knowing the build currently running
	// on the device, which not all test cases know during start. Store the
	// one true client here, and pass around pointers to the various
	// functions that may use it or device.Client to interact with the
	// target. All OTA attempts must first Close and nil out an existing
	// rpcClient and replace it with a new one after reboot. The final
	// rpcClient, if present, will be closed by the defer here.
	var rpcClient *sl4f.Client
	defer func() {
		if rpcClient != nil {
			rpcClient.Close()
		}
	}()

	if c.ShouldRepaveDevice() {
		rpcClient, err = paveDevice(ctx, device)
		if err != nil {
			log.Fatalf("failed to pave device: %s", err)
		}
	}

	testOTAs(t, ctx, device, &rpcClient)
}

func testOTAs(t *testing.T, ctx context.Context, device *device.Client, rpcClient **sl4f.Client) {
	for i := 1; i <= c.cycleCount; i++ {
		log.Printf("OTA Attempt %d", i)

		if err := doTestOTAs(ctx, device, rpcClient); err != nil {
			log.Fatalf("OTA Attempt %d failed: %s", i, err)
		}
	}
}

func doTestOTAs(ctx context.Context, device *device.Client, rpcClient **sl4f.Client) error {
	log.Printf("Starting OTA test cycle. Time out in %s", c.cycleTimeout)

	startTime := time.Now()
	ctx, cancel := context.WithDeadline(ctx, startTime.Add(c.cycleTimeout))
	defer cancel()

	outputDir, cleanup, err := c.archiveConfig.OutputDir()
	if err != nil {
		return fmt.Errorf("failed to get output directory: %s", err)
	}
	defer cleanup()

	repo, err := c.GetUpgradeRepository(ctx, outputDir)
	if err != nil {
		return fmt.Errorf("failed to get upgrade repository: %s", err)
	}

	// Install version N on the device if it is not already on that version.
	expectedSystemImageMerkle, err := repo.LookupUpdateSystemImageMerkle()
	if err != nil {
		return fmt.Errorf("error extracting expected system image merkle: %s", err)
	}

	upToDate, err := isDeviceUpToDate(ctx, device, *rpcClient, expectedSystemImageMerkle)
	if err != nil {
		return fmt.Errorf("failed to check if device is up to date: %s", err)
	}
	if !upToDate {
		log.Printf("starting OTA from N-1 -> N test")
		otaTime := time.Now()
		if err := systemOTA(ctx, device, rpcClient, repo, true); err != nil {
			return fmt.Errorf("OTA from N-1 -> N failed: %s", err)
		}
		log.Printf("OTA from N-1 -> N successful in %s", time.Now().Sub(otaTime))
	}

	log.Printf("starting OTA N -> N' test")
	otaTime := time.Now()
	if err := systemPrimeOTA(ctx, device, rpcClient, repo, false); err != nil {
		return fmt.Errorf("OTA from N -> N' failed: %s", err)
	}
	log.Printf("OTA from N -> N' successful in %s", time.Now().Sub(otaTime))

	log.Printf("starting OTA N' -> N test")
	otaTime = time.Now()
	if err := systemOTA(ctx, device, rpcClient, repo, false); err != nil {
		return fmt.Errorf("OTA from N' -> N failed: %s", err)
	}
	log.Printf("OTA from N' -> N successful in %s", time.Now().Sub(otaTime))
	log.Printf("OTA cycle sucessful in %s", time.Now().Sub(startTime))

	return nil
}

func paveDevice(ctx context.Context, device *device.Client) (*sl4f.Client, error) {
	log.Printf("Starting to pave device. Time out in %s", c.paveTimeout)

	startTime := time.Now()
	ctx, cancel := context.WithDeadline(ctx, startTime.Add(c.paveTimeout))
	defer cancel()

	outputDir, cleanup, err := c.archiveConfig.OutputDir()
	if err != nil {
		return nil, err
	}
	defer cleanup()

	downgradePaver, err := c.GetDowngradePaver(ctx, outputDir)
	if err != nil {
		return nil, fmt.Errorf("error getting downgrade paver: %s", err)
	}

	downgradeRepo, err := c.GetDowngradeRepository(ctx, outputDir)
	if err != nil {
		return nil, fmt.Errorf("error etting downgrade repository: %s", err)
	}

	log.Printf("starting pave")

	expectedSystemImageMerkle, err := downgradeRepo.LookupUpdateSystemImageMerkle()
	if err != nil {
		return nil, fmt.Errorf("error extracting expected system image merkle: %s", err)
	}

	// Reboot the device into recovery and pave it.
	if err = device.RebootToRecovery(ctx); err != nil {
		return nil, fmt.Errorf("failed to reboot to recovery: %s", err)
	}

	if err = downgradePaver.Pave(ctx, c.deviceConfig.DeviceName); err != nil {
		return nil, fmt.Errorf("device failed to pave: %s", err)
	}

	// Wait for the device to come online.
	if err = device.WaitForDeviceToBeConnected(ctx); err != nil {
		return nil, fmt.Errorf("device failed to connect: %s", err)
	}

	rpcClient, err := device.StartRpcSession(ctx, downgradeRepo)
	if err != nil {
		// FIXME(40913): every downgrade builder should at least build
		// sl4f as a universe package.
		log.Printf("unable to connect to sl4f after pave: %s", err)
		//log.Fatalf("unable to connect to sl4f after pave: %s", err)
	}

	// We always boot into the A partition after a pave.
	expectedConfig := sl4f.ConfigurationA

	if err := validateDevice(ctx, device, rpcClient, downgradeRepo, expectedSystemImageMerkle, &expectedConfig, true); err != nil {
		return nil, err
	}

	log.Printf("paving successful in %s", time.Now().Sub(startTime))

	return rpcClient, nil
}

// FIXME(45156) In order to ease landing Omaha, we're temporarily disabling the
// OTA test from talking directly to the update system, and instead are just
// directly calling out to the `system_updater` like we do for the N->N'
// upgrade. We need to do this because Omaha doesn't have a way to customize
// which server we are talking to, so we wouldn't be able to tell it to upgrade
// to the version of Fuchsia we want to test.
//
// We can revert back to this code once we've figured out how to implement this
// customization.
func systemOTA(
	ctx context.Context,
	device *device.Client,
	rpcClient **sl4f.Client,
	repo *packages.Repository,
	checkABR bool,
) error {
	expectedSystemImageMerkle, err := repo.LookupUpdateSystemImageMerkle()
	if err != nil {
		return fmt.Errorf("error extracting expected system image merkle: %s", err)
	}

	expectedConfig, err := determineTargetConfig(ctx, *rpcClient)
	if err != nil {
		return fmt.Errorf("error determining target config: %s", err)
	}

	upToDate, err := isDeviceUpToDate(ctx, device, *rpcClient, expectedSystemImageMerkle)
	if err != nil {
		return fmt.Errorf("failed to check if device is up to date: %s", err)
	}
	if upToDate {
		return fmt.Errorf("device already updated to the expected version %q", expectedSystemImageMerkle)
	}

	server, err := device.ServePackageRepository(ctx, repo, "upgrade_test")
	if err != nil {
		return fmt.Errorf("error setting up server: %s", err)
	}
	defer server.Shutdown(ctx)

	if err := device.TriggerSystemOTA(ctx, repo, rpcClient); err != nil {
		return fmt.Errorf("OTA failed: %s", err)
	}

	log.Printf("OTA complete, validating device")
	if err := validateDevice(ctx, device, *rpcClient, repo, expectedSystemImageMerkle, expectedConfig, checkABR); err != nil {
		return fmt.Errorf("failed to validate after OTA: %s", err)
	}

	return nil
}

/*
func systemOTA(ctx context.Context, device *device.Client, rpcClient **sl4f.Client, repo *packages.Repository, checkABR bool) error {
	expectedSystemImageMerkle, err := repo.LookupUpdateSystemImageMerkle()
	if err != nil {
		return fmt.Errorf("error extracting expected system image merkle: %s", err)
	}

	return otaToPackage(
		ctx,
		device,
		rpcClient,
		repo,
		expectedSystemImageMerkle,
		"fuchsia-pkg://fuchsia.com/update",
		checkABR,
	)
}
*/

func systemPrimeOTA(ctx context.Context, device *device.Client, rpcClient **sl4f.Client, repo *packages.Repository, checkABR bool) error {
	expectedSystemImageMerkle, err := repo.LookupUpdatePrimeSystemImageMerkle()
	if err != nil {
		return fmt.Errorf("error extracting expected system image merkle: %s", err)
	}

	return otaToPackage(
		ctx,
		device,
		rpcClient,
		repo,
		expectedSystemImageMerkle,
		"fuchsia-pkg://fuchsia.com/update_prime",
		checkABR,
	)
}

func otaToPackage(
	ctx context.Context,
	device *device.Client,
	rpcClient **sl4f.Client,
	repo *packages.Repository,
	expectedSystemImageMerkle string,
	updatePackageUrl string,
	checkABR bool,
) error {
	expectedConfig, err := determineTargetConfig(ctx, *rpcClient)
	if err != nil {
		return fmt.Errorf("error determining target config: %s", err)
	}

	upToDate, err := isDeviceUpToDate(ctx, device, *rpcClient, expectedSystemImageMerkle)
	if err != nil {
		return fmt.Errorf("failed to check if device is up to date: %s", err)
	}
	if upToDate {
		return fmt.Errorf("device already updated to the expected version %q", expectedSystemImageMerkle)
	}

	server, err := device.ServePackageRepository(ctx, repo, "upgrade_test")
	if err != nil {
		return fmt.Errorf("error setting up server: %s", err)
	}
	defer server.Shutdown(ctx)

	// In order to manually trigger the system updater, we need the `run`
	// package. Since builds can be configured to not automatically install
	// packages, we need to explicitly resolve it.
	err = device.Run(ctx, "pkgctl resolve fuchsia-pkg://fuchsia.com/run/0", os.Stdout, os.Stderr)
	if err != nil {
		return fmt.Errorf("error resolving the run package: %v", err)
	}

	log.Printf("Starting system OTA")
	startTime := time.Now()

	cmd := fmt.Sprintf("run \"fuchsia-pkg://fuchsia.com/amber#meta/system_updater.cmx\" --initiator manual --reboot=false --update \"%s\"", updatePackageUrl)
	if err = device.Run(ctx, cmd, os.Stdout, os.Stderr); err != nil {
		return fmt.Errorf("failed to run system_updater.cmx: %s", err)
	}

	log.Printf("Update successful, rebooting now")

	if err = device.Reboot(ctx, repo, rpcClient); err != nil {
		return fmt.Errorf("device failed to reboot after OTA applied: %s", err)
	}

	log.Printf("OTA complete in in %s", time.Now().Sub(startTime))

	log.Printf("Validating device")
	// FIXME: See comment in device.TriggerSystemOTA()
	if *rpcClient != nil {
		(*rpcClient).Close()
		*rpcClient = nil
	}
	*rpcClient, err = device.StartRpcSession(ctx, repo)
	if err != nil {
		return fmt.Errorf("unable to connect to sl4f after OTA: %s", err)
	}
	if err := validateDevice(ctx, device, *rpcClient, repo, expectedSystemImageMerkle, expectedConfig, checkABR); err != nil {
		return fmt.Errorf("failed to validate after OTA: %s", err)
	}
	return nil
}

func isDeviceUpToDate(ctx context.Context, device *device.Client, rpcClient *sl4f.Client, expectedSystemImageMerkle string) (bool, error) {
	// Get the device's current /system/meta. Error out if it is the same
	// version we are about to OTA to.
	var remoteSystemImageMerkle string
	var err error
	if rpcClient == nil {
		remoteSystemImageMerkle, err = device.GetSystemImageMerkle(ctx)
	} else {
		remoteSystemImageMerkle, err = rpcClient.GetSystemImageMerkle(ctx)
	}
	if err != nil {
		return false, err
	}
	log.Printf("current system image merkle: %q", remoteSystemImageMerkle)
	log.Printf("upgrading to system image merkle: %q", expectedSystemImageMerkle)

	return expectedSystemImageMerkle == remoteSystemImageMerkle, nil
}

func determineTargetConfig(ctx context.Context, rpcClient *sl4f.Client) (*sl4f.Configuration, error) {
	if rpcClient == nil {
		log.Printf("sl4f not running, cannot determine current active partition")
		return nil, nil
	}
	activeConfig, err := rpcClient.PaverQueryActiveConfiguration(ctx)
	if err == sl4f.ErrNotSupported {
		log.Printf("device does not support ABR")
		return nil, nil
	} else if err != nil {
		return nil, err
	}

	log.Printf("device booted to slot %s", activeConfig)

	var targetConfig sl4f.Configuration
	if activeConfig == sl4f.ConfigurationA {
		targetConfig = sl4f.ConfigurationB
	} else {
		targetConfig = sl4f.ConfigurationA
	}

	return &targetConfig, nil
}

func validateDevice(
	ctx context.Context,
	device *device.Client,
	rpcClient *sl4f.Client,
	repo *packages.Repository,
	expectedSystemImageMerkle string,
	expectedConfig *sl4f.Configuration,
	checkABR bool,
) error {
	// At the this point the system should have been updated to the target
	// system version. Confirm the update by fetching the device's current
	// /system/meta, and making sure it is the correct version.
	upToDate, err := isDeviceUpToDate(ctx, device, rpcClient, expectedSystemImageMerkle)
	if err != nil {
		return fmt.Errorf("failed to check if device is up to date: %s", err)
	}
	if !upToDate {
		return fmt.Errorf("system version failed to update to %q", expectedSystemImageMerkle)
	}

	// Make sure the device doesn't have any broken static packages.
	// FIXME(40913): every builder should at least build sl4f as a universe package.
	if rpcClient == nil {
		if err := device.ValidateStaticPackages(ctx); err != nil {
			return fmt.Errorf("failed to validate static packages: %s", err)
		}
	} else {
		if err := rpcClient.ValidateStaticPackages(ctx); err != nil {
			return fmt.Errorf("failed to validate static packages: %s", err)
		}

		// Ensure the device is booting from the expected boot slot
		activeConfig, err := rpcClient.PaverQueryActiveConfiguration(ctx)
		if err == sl4f.ErrNotSupported {
			log.Printf("device does not support querying the active configuration")
		} else if err != nil {
			return fmt.Errorf("unable to determine active boot configuration: %s", err)
		}

		log.Printf("device booted to slot %s", activeConfig)

		if expectedConfig != nil && activeConfig != *expectedConfig {
			// FIXME(43336): during the rollout of ABR, the N-1 build might
			// not be writing to the inactive partition, so don't
			// err out during that phase. This will be removed once
			// ABR has rolled through GI.
			if checkABR {
				log.Printf("expected device to boot from slot %s, got %s (ignoring during ABR rollout)", *expectedConfig, activeConfig)
			} else {
				return fmt.Errorf("expected device to boot from slot %s, got %s", *expectedConfig, activeConfig)
			}
		}
	}

	return nil
}
