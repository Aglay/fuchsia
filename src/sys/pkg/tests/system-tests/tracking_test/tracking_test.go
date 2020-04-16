// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package tracking

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
	"fuchsia.googlesource.com/system_tests/check"
	"fuchsia.googlesource.com/system_tests/pave"
	"golang.org/x/crypto/ssh"
)

var c *config

func TestMain(m *testing.M) {
	log.SetPrefix("tracking-test: ")
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

func TestOTA(t *testing.T) {
	ctx := context.Background()

	device, err := c.deviceConfig.NewDeviceClient(ctx)
	if err != nil {
		t.Fatalf("failed to create ota test client: %s", err)
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

	if c.shouldRepaveDevice() {
		rpcClient, err = paveDevice(ctx, device)
		if err != nil {
			t.Fatalf("failed to pave device: %s", err)
		}
	}

	testTrackingOTAs(t, ctx, device, &rpcClient)
}

func testTrackingOTAs(t *testing.T, ctx context.Context, device *device.Client, rpcClient **sl4f.Client) {
	builder, err := c.getUpgradeBuilder()
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

		buildID, err := builder.GetLatestBuildID(ctx)
		if err != nil {
			t.Fatalf("error getting latest build for builder %s: %s", builder, err)
		}

		if buildID == lastBuildID {
			log.Printf("already updated to %s, sleeping", buildID)
			time.Sleep(60 * time.Second)
			continue
		}
		log.Printf("Tracking Test Attempt %d upgrading from build %s to build %s", attempt, lastBuildID, buildID)

		if err := testTrackingOTAAttempt(ctx, device, rpcClient, buildID, checkABR); err != nil {
			t.Fatalf("Tracking Test Attempt %d failed: %s", attempt, err)
		}

		log.Printf("Tracking Test Attempt %d successful", attempt)
		log.Printf("------------------------------------------------------------------------------")

		checkABR = true
		lastBuildID = buildID
		attempt += 1
	}
}

func testTrackingOTAAttempt(
	ctx context.Context,
	device *device.Client,
	rpcClient **sl4f.Client,
	buildID string,
	checkABR bool,
) error {
	ctx, cancel := context.WithTimeout(ctx, c.cycleTimeout)
	defer cancel()

	outputDir, cleanup, err := c.archiveConfig.OutputDir()
	if err != nil {
		return fmt.Errorf("failed to get output directory: %s", err)
	}
	defer cleanup()

	build, err := c.archiveConfig.BuildArchive().GetBuildByID(ctx, buildID, outputDir, nil)
	if err != nil {
		return fmt.Errorf("failed to find build %s: %s", buildID, err)
	}

	repo, err := build.GetPackageRepository(ctx)
	if err != nil {
		return fmt.Errorf("failed to get repo for build: %s", err)
	}

	expectedSystemImageMerkle, err := repo.LookupUpdateSystemImageMerkle()
	if err != nil {
		return fmt.Errorf("failed to get repo system image merkle: %s", err)
	}

	upToDate, err := check.IsDeviceUpToDate(ctx, device, expectedSystemImageMerkle)
	if err != nil {
		return fmt.Errorf("failed to check if device is up to date: %s", err)
	}
	if upToDate {
		log.Printf("device already up to date")
		return nil
	}

	log.Printf("\n\n")
	log.Printf("OTAing to %s", build)

	return systemOTA(ctx, device, rpcClient, repo, checkABR)
}

func paveDevice(ctx context.Context, device *device.Client) (*sl4f.Client, error) {
	ctx, cancel := context.WithTimeout(ctx, c.paveTimeout)
	defer cancel()

	outputDir, cleanup, err := c.archiveConfig.OutputDir()
	if err != nil {
		return nil, err
	}
	defer cleanup()

	downgradeBuild, err := c.getDowngradeBuild(ctx, outputDir)
	if err != nil {
		return nil, fmt.Errorf("failed to get downgrade build: %w", err)
	}

	downgradeRepo, err := downgradeBuild.GetPackageRepository(ctx)
	if err != nil {
		return nil, fmt.Errorf("error getting downgrade repository: %w", err)
	}

	log.Printf("starting pave")

	expectedSystemImageMerkle, err := downgradeRepo.LookupUpdateSystemImageMerkle()
	if err != nil {
		return nil, fmt.Errorf("error extracting expected system image merkle: %s", err)
	}

	if err := pave.PaveDevice(ctx, device, downgradeBuild, c.otaToRecovery); err != nil {
		return nil, fmt.Errorf("failed to pave device during initialization: %w", err)
	}

	rpcClient, err := device.StartRpcSession(ctx, downgradeRepo)
	if err != nil {
		// FIXME(40913): every downgrade builder should at least build
		// sl4f as a universe package.
		log.Printf("unable to connect to sl4f after pave: %s", err)
		//t.Fatalf("unable to connect to sl4f after pave: %s", err)
	}

	// We always boot into the A partition after a pave.
	expectedConfig := sl4f.ConfigurationA

	if err := check.ValidateDevice(
		ctx,
		device,
		rpcClient,
		expectedSystemImageMerkle,
		&expectedConfig,
		true,
	); err != nil {
		return nil, err
	}

	log.Printf("paving successful")

	return rpcClient, nil
}

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

func otaToPackage(
	ctx context.Context,
	device *device.Client,
	rpcClient **sl4f.Client,
	repo *packages.Repository,
	expectedSystemImageMerkle string,
	updatePackageUrl string,
	checkABR bool,
) error {
	expectedConfig, err := check.DetermineTargetABRConfig(ctx, *rpcClient)
	if err != nil {
		return fmt.Errorf("error determining target config: %s", err)
	}

	upToDate, err := check.IsDeviceUpToDate(ctx, device, expectedSystemImageMerkle)
	if err != nil {
		return fmt.Errorf("failed to check if device is up to date: %s", err)
	}
	if upToDate {
		return fmt.Errorf("device already updated to the expected version %q", expectedSystemImageMerkle)
	}

	server, err := device.ServePackageRepository(ctx, repo, "tracking_test", true)
	if err != nil {
		return fmt.Errorf("error setting up server: %s", err)
	}
	defer server.Shutdown(ctx)

	// In order to manually trigger the system updater, we need the `run`
	// package. Since builds can be configured to not automatically install
	// packages, we need to explicitly resolve it.
	cmd := []string{"pkgctl", "resolve", "fuchsia-pkg://fuchsia.com/run/0"}
	if err := device.Run(ctx, cmd, os.Stdout, os.Stderr); err != nil {
		return fmt.Errorf("error resolving the run package: %v", err)
	}

	ch := make(chan struct{})
	device.RegisterDisconnectListener(ch)

	log.Printf("starting system OTA")

	cmd = []string{
		"run",
		"\"fuchsia-pkg://fuchsia.com/amber#meta/system_updater.cmx\"",
		"--update",
		updatePackageUrl,
		"&&",
		"sleep",
		"60",
	}
	if err := device.Run(ctx, cmd, os.Stdout, os.Stderr); err != nil {
		if _, ok := err.(*ssh.ExitMissingError); !ok {
			return fmt.Errorf("failed to run system_updater.cmx: %s", err)
		}
	}

	// Wait until we get a signal that we have disconnected
	select {
	case <-ch:
	case <-ctx.Done():
		return fmt.Errorf("device did not disconnect: %s", ctx.Err())
	}

	if err = device.Reconnect(ctx); err != nil {
		return fmt.Errorf("device failed to connect: %s", err)
	}

	log.Printf("OTA complete, validating device")
	// FIXME: See comment in device.TriggerSystemOTA()
	if *rpcClient != nil {
		(*rpcClient).Close()
		*rpcClient = nil
	}
	*rpcClient, err = device.StartRpcSession(ctx, repo)
	if err != nil {
		return fmt.Errorf("unable to connect to sl4f after OTA: %s", err)
	}
	if err := check.ValidateDevice(
		ctx,
		device,
		*rpcClient,
		expectedSystemImageMerkle,
		expectedConfig,
		checkABR,
	); err != nil {
		return fmt.Errorf("failed to validate after OTA: %s", err)
	}
	return nil
}
