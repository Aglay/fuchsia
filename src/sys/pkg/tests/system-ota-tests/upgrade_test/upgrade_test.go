// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package upgrade

import (
	"context"
	"flag"
	"log"
	"os"
	"sync"
	"testing"
	"time"

	"fuchsia.googlesource.com/host_target_testing/device"
	"fuchsia.googlesource.com/host_target_testing/packages"
	"fuchsia.googlesource.com/host_target_testing/sl4f"

	"golang.org/x/crypto/ssh"
)

var c *Config

func TestMain(m *testing.M) {
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

	device, err := c.NewDeviceClient()
	if err != nil {
		t.Fatalf("failed to create ota test client: %s", err)
	}
	defer device.Close()

	// Creating a sl4f.Client requires knowing the build currently running
	// on the device, which not all test cases know during start. Store the
	// one true client here, and pass around pointers to the various
	// functions that may use it or device.Client to interact with the
	// targer. All OTA attempts must first Close and nil out an existing
	// rpcClient and replace it with a new one after reboot. The final
	// rpcClient, if present, will be closed by the defer here.
	var rpcClient *sl4f.Client
	defer func() {
		if rpcClient != nil {
			rpcClient.Close()
		}
	}()

	if c.ShouldRepaveDevice() {
		rpcClient = paveDevice(t, ctx, device)
	}

	if c.LongevityTest {
		testLongevityOTAs(t, ctx, device, &rpcClient)
	} else {
		testOTAs(t, ctx, device, &rpcClient)
	}
}

func testOTAs(t *testing.T, ctx context.Context, device *device.Client, rpcClient **sl4f.Client) {
	ctx, cancel := context.WithTimeout(ctx, c.cycleTimeout)
	defer cancel()

	ch := make(chan struct{})
	go func() {
		doTestOTAs(t, ctx, device, rpcClient)
		ch <- struct{}{}
	}()

	select {
	case <-ch:
	case <-ctx.Done():
		t.Fatalf("OTA Cycle timed out: %s", ctx.Err())
	}
}

func doTestOTAs(t *testing.T, ctx context.Context, device *device.Client, rpcClient **sl4f.Client) {
	outputDir, cleanup, err := c.OutputDir()
	if err != nil {
		t.Fatal(err)
	}
	defer cleanup()

	repo, err := c.GetUpgradeRepository(outputDir)
	if err != nil {
		t.Fatal(err)
	}

	// Install version N on the device if it is not already on that version.
	expectedSystemImageMerkle, err := repo.LookupUpdateSystemImageMerkle()
	if err != nil {
		t.Fatalf("error extracting expected system image merkle: %s", err)
	}

	if !isDeviceUpToDate(t, ctx, device, *rpcClient, expectedSystemImageMerkle) {
		log.Printf("\n\n")
		log.Printf("starting OTA from N-1 -> N test")
		systemOTA(t, ctx, device, rpcClient, repo, true)
		log.Printf("OTA from N-1 -> N successful")
	}

	log.Printf("\n\n")
	log.Printf("starting OTA N -> N' test")
	systemPrimeOTA(t, ctx, device, rpcClient, repo, false)
	log.Printf("OTA from N -> N' successful")

	log.Printf("\n\n")
	log.Printf("starting OTA N' -> N test")
	systemOTA(t, ctx, device, rpcClient, repo, false)
	log.Printf("OTA from N' -> N successful")
}

func testLongevityOTAs(t *testing.T, ctx context.Context, device *device.Client, rpcClient **sl4f.Client) {
	builder, err := c.GetUpgradeBuilder()
	if err != nil {
		t.Fatal(err)
	}

	// We only check ABR after the first update, since we can't be sure if
	// the initial version of Fuchsia is recent enough to support ABR, but
	// it should support ABR after the first OTA.
	checkABR := false

	lastBuildID := ""
	attempt := 1
	for {
		log.Printf("Look up latest build for builder %s", builder)

		buildID, err := builder.GetLatestBuildID()
		if err != nil {
			t.Fatalf("error getting latest build for builder %s: %s", builder, err)
		}

		if buildID == lastBuildID {
			log.Printf("already updated to %s, sleeping", buildID)
			time.Sleep(60 * time.Second)
			continue
		}
		log.Printf("Longevity Test Attempt %d  upgrading from build %s to build %s", attempt, lastBuildID, buildID)

		testLongevityOTAAttempt(t, ctx, device, rpcClient, buildID, checkABR)

		log.Printf("Longevity Test Attempt %d successful", attempt)
		log.Printf("------------------------------------------------------------------------------")

		checkABR = true
		lastBuildID = buildID
		attempt += 1
	}
}

func testLongevityOTAAttempt(t *testing.T, ctx context.Context, device *device.Client, rpcClient **sl4f.Client, buildID string, checkABR bool) {
	ctx, cancel := context.WithTimeout(ctx, c.cycleTimeout)
	defer cancel()

	ch := make(chan struct{})
	go func() {
		doTestLongevityOTAAttempt(t, ctx, device, rpcClient, buildID, checkABR)
		ch <- struct{}{}
	}()

	select {
	case <-ch:
	case <-ctx.Done():
		t.Fatalf("OTA Cycle timed out: %s", ctx.Err())
	}
}

func doTestLongevityOTAAttempt(t *testing.T, ctx context.Context, device *device.Client, rpcClient **sl4f.Client, buildID string, checkABR bool) {
	outputDir, cleanup, err := c.OutputDir()
	if err != nil {
		t.Fatal(err)
	}
	defer cleanup()

	build, err := c.BuildArchive().GetBuildByID(buildID, outputDir)
	if err != nil {
		t.Fatalf("failed to find build %s: %s", buildID, err)
	}

	repo, err := build.GetPackageRepository()
	if err != nil {
		t.Fatalf("failed to get repo for build: %s", err)
	}

	expectedSystemImageMerkle, err := repo.LookupUpdateSystemImageMerkle()
	if err != nil {
		t.Fatal(err)
	}

	if isDeviceUpToDate(t, ctx, device, *rpcClient, expectedSystemImageMerkle) {
		log.Printf("device already up to date")
		return
	}

	log.Printf("\n\n")
	log.Printf("OTAing to %s", build)
	systemOTA(t, ctx, device, rpcClient, repo, checkABR)
}

func paveDevice(t *testing.T, ctx context.Context, device *device.Client) *sl4f.Client {
	ctx, cancel := context.WithTimeout(ctx, c.paveTimeout)
	defer cancel()

	ch := make(chan *sl4f.Client)
	go func() {
		ch <- doPaveDevice(t, ctx, device)
	}()

	var rpcClient *sl4f.Client

	select {
	case rpcClient = <-ch:
	case <-ctx.Done():
		t.Fatalf("Paving timed out: %s", ctx.Err())
	}

	return rpcClient
}

func doPaveDevice(t *testing.T, ctx context.Context, device *device.Client) *sl4f.Client {
	outputDir, cleanup, err := c.OutputDir()
	if err != nil {
		t.Fatal(err)
	}
	defer cleanup()

	downgradePaver, err := c.GetDowngradePaver(outputDir)
	if err != nil {
		t.Fatal(err)
	}

	downgradeRepo, err := c.GetDowngradeRepository(outputDir)
	if err != nil {
		t.Fatal(err)
	}

	log.Printf("starting pave")

	expectedSystemImageMerkle, err := downgradeRepo.LookupUpdateSystemImageMerkle()
	if err != nil {
		t.Fatalf("error extracting expected system image merkle: %s", err)
	}

	// Reboot the device into recovery and pave it.
	if err = device.RebootToRecovery(); err != nil {
		t.Fatalf("failed to reboot to recovery: %s", err)
	}

	if err = downgradePaver.Pave(c.DeviceName); err != nil {
		t.Fatalf("device failed to pave: %s", err)
	}

	// Wait for the device to come online.
	device.WaitForDeviceToBeConnected()

	rpcClient, err := device.StartRpcSession(ctx, downgradeRepo)
	if err != nil {
		// FIXME(40913): every downgrade builder should at least build
		// sl4f as a universe package.
		log.Printf("unable to connect to sl4f after pave: %s", err)
		//t.Fatalf("unable to connect to sl4f after pave: %s", err)
	}

	// We always boot into the A partition after a pave.
	expectedConfig := sl4f.ConfigurationA

	validateDevice(t, ctx, device, rpcClient, downgradeRepo, expectedSystemImageMerkle, &expectedConfig, true)

	log.Printf("paving successful")

	return rpcClient
}

func systemOTA(t *testing.T, ctx context.Context, device *device.Client, rpcClient **sl4f.Client, repo *packages.Repository, checkABR bool) {
	expectedSystemImageMerkle, err := repo.LookupUpdateSystemImageMerkle()
	if err != nil {
		t.Fatalf("error extracting expected system image merkle: %s", err)
	}

	expectedConfig, err := determineTargetConfig(ctx, *rpcClient)
	if err != nil {
		t.Fatalf("error determining target config: %s", err)
	}

	if isDeviceUpToDate(t, ctx, device, *rpcClient, expectedSystemImageMerkle) {
		t.Fatalf("device already updated to the expected version %q", expectedSystemImageMerkle)
	}

	server, err := device.ServePackageRepository(ctx, repo, "upgrade_test")
	if err != nil {
		t.Fatalf("error setting up server: %s", err)
	}
	defer server.Shutdown(ctx)

	if err := device.TriggerSystemOTA(ctx, repo, rpcClient); err != nil {
		t.Fatalf("OTA failed: %s", err)
	}

	log.Printf("OTA complete, validating device")
	validateDevice(t, ctx, device, *rpcClient, repo, expectedSystemImageMerkle, expectedConfig, checkABR)
}

func systemPrimeOTA(t *testing.T, ctx context.Context, device *device.Client, rpcClient **sl4f.Client, repo *packages.Repository, checkABR bool) {
	expectedSystemImageMerkle, err := repo.LookupUpdatePrimeSystemImageMerkle()
	if err != nil {
		t.Fatalf("error extracting expected system image merkle: %s", err)
	}

	expectedConfig, err := determineTargetConfig(ctx, *rpcClient)
	if err != nil {
		t.Fatalf("error determining target config: %s", err)
	}

	if isDeviceUpToDate(t, ctx, device, *rpcClient, expectedSystemImageMerkle) {
		t.Fatalf("device already updated to the expected version %q", expectedSystemImageMerkle)
	}

	server, err := device.ServePackageRepository(ctx, repo, "upgrade_test")
	if err != nil {
		t.Fatalf("error setting up server: %s", err)
	}
	defer server.Shutdown(ctx)

	// Since we're invoking system_updater.cmx directly, we need to do the GC ourselves
	// FIXME(40913): every downgrade builder should at least build
	// sl4f as a universe package, which would ensure rpcClient is non-nil here.
	if *rpcClient == nil {
		err = device.DeleteRemotePath("/pkgfs/ctl/garbage")
	} else {
		err = (*rpcClient).FileDelete(ctx, "/pkgfs/ctl/garbage")
	}
	if err != nil {
		t.Fatalf("error running GC: %v", err)
	}

	// In order to manually trigger the system updater, we need the `run`
	// package. Since builds can be configured to not automatically install
	// packages, we need to explicitly resolve it.
	err = device.Run("pkgctl resolve fuchsia-pkg://fuchsia.com/run/0", os.Stdout, os.Stderr)
	if err != nil {
		t.Fatalf("error resolving the run package: %v", err)
	}

	var wg sync.WaitGroup
	device.RegisterDisconnectListener(&wg)

	log.Printf("starting system OTA")

	err = device.Run(`run "fuchsia-pkg://fuchsia.com/amber#meta/system_updater.cmx" --update "fuchsia-pkg://fuchsia.com/update_prime" && sleep 60`, os.Stdout, os.Stderr)
	if err != nil {
		if _, ok := err.(*ssh.ExitMissingError); !ok {
			t.Fatalf("failed to run system_updater.cmx: %s", err)
		}
	}

	// Wait until we get a signal that we have disconnected
	wg.Wait()

	device.WaitForDeviceToBeConnected()

	log.Printf("OTA complete, validating device")
	// FIXME: See comment in device.TriggerSystemOTA()
	if *rpcClient != nil {
		(*rpcClient).Close()
		*rpcClient = nil
	}
	*rpcClient, err = device.StartRpcSession(ctx, repo)
	if err != nil {
		t.Fatalf("unable to connect to sl4f after OTA: %s", err)
	}
	validateDevice(t, ctx, device, *rpcClient, repo, expectedSystemImageMerkle, expectedConfig, checkABR)
}

func isDeviceUpToDate(t *testing.T, ctx context.Context, device *device.Client, rpcClient *sl4f.Client, expectedSystemImageMerkle string) bool {
	// Get the device's current /system/meta. Error out if it is the same
	// version we are about to OTA to.
	var remoteSystemImageMerkle string
	var err error
	if rpcClient == nil {
		remoteSystemImageMerkle, err = device.GetSystemImageMerkle()
	} else {
		remoteSystemImageMerkle, err = rpcClient.GetSystemImageMerkle(ctx)
	}
	if err != nil {
		t.Fatal(err)
	}
	log.Printf("current system image merkle: %q", remoteSystemImageMerkle)
	log.Printf("upgrading to system image merkle: %q", expectedSystemImageMerkle)

	return expectedSystemImageMerkle == remoteSystemImageMerkle
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

func validateDevice(t *testing.T, ctx context.Context, device *device.Client, rpcClient *sl4f.Client, repo *packages.Repository, expectedSystemImageMerkle string, expectedConfig *sl4f.Configuration, checkABR bool) {
	// At the this point the system should have been updated to the target
	// system version. Confirm the update by fetching the device's current
	// /system/meta, and making sure it is the correct version.
	if !isDeviceUpToDate(t, ctx, device, rpcClient, expectedSystemImageMerkle) {
		t.Fatalf("system version failed to update to %q", expectedSystemImageMerkle)
	}

	// Make sure the device doesn't have any broken static packages.
	// FIXME(40913): every builder should at least build sl4f as a universe package.
	if rpcClient == nil {
		if err := device.ValidateStaticPackages(); err != nil {
			t.Fatal(err)
		}
	} else {
		if err := rpcClient.ValidateStaticPackages(ctx); err != nil {
			t.Fatal(err)
		}

		// Ensure the device is booting from the expected boot slot
		activeConfig, err := rpcClient.PaverQueryActiveConfiguration(ctx)
		if err == sl4f.ErrNotSupported {
			log.Printf("device does not support querying the active configuration")
		} else if err != nil {
			t.Fatalf("unable to determine active boot configuration: %s", err)
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
				t.Fatalf("expected device to boot from slot %s, got %s", *expectedConfig, activeConfig)
			}
		}
	}
}
