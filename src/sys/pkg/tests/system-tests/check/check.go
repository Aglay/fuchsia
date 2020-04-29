// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package check

import (
	"context"
	"fmt"

	"fuchsia.googlesource.com/host_target_testing/device"
	"fuchsia.googlesource.com/host_target_testing/sl4f"

	"go.fuchsia.dev/fuchsia/tools/lib/logger"
)

// IsDeviceUpToDate checks if the device's /system/meta matches the expected
// system image merkle.
func IsDeviceUpToDate(
	ctx context.Context,
	device *device.Client,
	expectedSystemImageMerkle string,
) (bool, error) {
	remoteSystemImageMerkle, err := device.GetSystemImageMerkle(ctx)
	if err != nil {
		return false, fmt.Errorf("failed to get system image merkle while checking if device is up to date: %w", err)
	}

	logger.Infof(ctx, "current system image merkle:  %q", remoteSystemImageMerkle)
	logger.Infof(ctx, "expected system image merkle: %q", expectedSystemImageMerkle)

	return expectedSystemImageMerkle == remoteSystemImageMerkle, nil
}

func DetermineActiveABRConfig(
	ctx context.Context,
	rpcClient *sl4f.Client,
) (*sl4f.Configuration, error) {
	if rpcClient == nil {
		logger.Infof(ctx, "sl4f not running, cannot determine current active partition")
		return nil, nil
	}

	activeConfig, err := rpcClient.PaverQueryActiveConfiguration(ctx)
	if err == sl4f.ErrNotSupported {
		logger.Infof(ctx, "device does not support querying the active configuration")
		return nil, nil
	} else if err != nil {
		return nil, err
	}

	logger.Infof(ctx, "device booted to slot %s", activeConfig)

	return &activeConfig, nil
}

func DetermineTargetABRConfig(
	ctx context.Context,
	rpcClient *sl4f.Client,
) (*sl4f.Configuration, error) {
	activeConfig, err := DetermineActiveABRConfig(ctx, rpcClient)
	if err != nil {
		return nil, fmt.Errorf("could not determine target config when querying active config: %w", err)
	}
	if activeConfig == nil {
		return nil, nil
	}

	var targetConfig sl4f.Configuration
	if *activeConfig == sl4f.ConfigurationA {
		targetConfig = sl4f.ConfigurationB
	} else {
		targetConfig = sl4f.ConfigurationA
	}

	return &targetConfig, nil
}

func CheckABRConfig(
	ctx context.Context,
	rpcClient *sl4f.Client,
	expectedConfig *sl4f.Configuration,
) error {
	if expectedConfig == nil {
		logger.Infof(ctx, "no configuration expected, so not checking ABR configuration")
		return nil
	}

	if rpcClient == nil {
		logger.Infof(ctx, "not connected to sl4f, cannot check ABR configuration")
		return nil
	}

	// Ensure the device is booting from the expected boot slot.
	activeConfig, err := DetermineActiveABRConfig(ctx, rpcClient)
	if err != nil {
		return fmt.Errorf("unable to determine active boot configuration: %w", err)
	}

	if activeConfig == nil {
		return fmt.Errorf("expected device to boot from slot %q, got <nil>", *expectedConfig)
	} else if *activeConfig != *expectedConfig {
		return fmt.Errorf("expected device to boot from slot %q, got %q", *expectedConfig, *activeConfig)
	}

	return nil
}

func ValidateDevice(
	ctx context.Context,
	device *device.Client,
	rpcClient *sl4f.Client,
	expectedSystemImageMerkle string,
	expectedConfig *sl4f.Configuration,
	warnOnABR bool,
) error {
	// At the this point the system should have been updated to the target
	// system version. Confirm the update by fetching the device's current
	// /system/meta, and making sure it is the correct version.
	upToDate, err := IsDeviceUpToDate(ctx, device, expectedSystemImageMerkle)
	if err != nil {
		return fmt.Errorf("failed to check if device is up to date: %w", err)
	}
	if !upToDate {
		return fmt.Errorf("system version failed to update to %q", expectedSystemImageMerkle)
	}

	// Make sure the device doesn't have any broken static packages.
	// FIXME(40913): every builder should at least build sl4f as a universe package.
	if rpcClient == nil {
		if err := device.ValidateStaticPackages(ctx); err != nil {
			return fmt.Errorf("failed to validate static packages without sl4f: %w", err)
		}
	} else {
		if err := rpcClient.ValidateStaticPackages(ctx); err != nil {
			return fmt.Errorf("failed to validate static packages with sl4f: %w", err)
		}

		if err := CheckABRConfig(ctx, rpcClient, expectedConfig); err != nil {
			// FIXME(43336): during the rollout of ABR, the N-1 build might
			// not be writing to the inactive partition, so don't
			// err out during that phase. This will be removed once
			// ABR has rolled through GI.
			if warnOnABR {
				logger.Infof(ctx, "ignoring error during ABR rollout: %v", err)
			} else {
				return fmt.Errorf("failed to validate device: %w", err)
			}
		}
	}

	return nil
}
