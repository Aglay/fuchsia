// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package tracking

import (
	"context"
	"flag"
	"fmt"
	"os"
	"time"

	"go.fuchsia.dev/fuchsia/src/sys/pkg/testing/host-target-testing/artifacts"

	systemTestConfig "go.fuchsia.dev/fuchsia/src/sys/pkg/tests/system-tests/config"
)

type config struct {
	archiveConfig            *systemTestConfig.ArchiveConfig
	installerConfig          *systemTestConfig.InstallerConfig
	deviceConfig             *systemTestConfig.DeviceConfig
	otaToRecovery            bool
	downgradeBuilderName     string
	downgradeBuildID         string
	downgradeFuchsiaBuildDir string
	upgradeBuilderName       string
	upgradeBuildID           string
	upgradeFuchsiaBuildDir   string
	paveTimeout              time.Duration
	cycleCount               int
	cycleTimeout             time.Duration
}

func newConfig(fs *flag.FlagSet) (*config, error) {
	installerConfig, err := systemTestConfig.NewInstallerConfig(fs)
	if err != nil {
		return nil, err
	}

	c := &config{
		archiveConfig:   systemTestConfig.NewArchiveConfig(fs),
		installerConfig: installerConfig,
		deviceConfig:    systemTestConfig.NewDeviceConfig(fs),
	}

	fs.StringVar(&c.downgradeBuilderName, "downgrade-builder-name", "", "downgrade to the latest version of this builder")
	fs.StringVar(&c.downgradeBuildID, "downgrade-build-id", "", "downgrade to this specific build id")
	fs.StringVar(&c.downgradeFuchsiaBuildDir, "downgrade-fuchsia-build-dir", "", "Path to the downgrade fuchsia build dir")
	fs.StringVar(&c.upgradeBuilderName, "upgrade-builder-name", "", "upgrade to the latest version of this builder")
	fs.StringVar(&c.upgradeBuildID, "upgrade-build-id", os.Getenv("BUILDBUCKET_ID"), "upgrade to this build id (default is $BUILDBUCKET_ID)")
	fs.StringVar(&c.upgradeFuchsiaBuildDir, "upgrade-fuchsia-build-dir", "", "Path to the upgrade fuchsia build dir")
	fs.BoolVar(&c.otaToRecovery, "ota-to-recovery", false, "downgrade with an OTA instead of rebooting to recovery")
	fs.DurationVar(&c.paveTimeout, "pave-timeout", 1<<63-1, "Err if a pave takes longer than this time (default is no timeout)")
	fs.IntVar(&c.cycleCount, "cycle-count", 1, "How many cycles to run the test before completing (default is 1)")
	fs.DurationVar(&c.cycleTimeout, "cycle-timeout", 1<<63-1, "Err if a test cycle takes longer than this time (default is no timeout)")

	return c, nil
}

func (c *config) validate() error {
	defined := 0
	for _, s := range []string{
		c.downgradeBuilderName,
		c.downgradeBuildID,
		c.downgradeFuchsiaBuildDir,
	} {
		if s != "" {
			defined += 1
		}
	}
	if defined > 1 {
		return fmt.Errorf("-downgrade-builder-name, -downgrade-build-id, and -downgrade-fuchsia-build-dir are mutually exclusive")
	}

	defined = 0
	for _, s := range []string{c.upgradeBuilderName, c.upgradeBuildID, c.upgradeFuchsiaBuildDir} {
		if s != "" {
			defined += 1
		}
	}
	if defined != 1 {
		return fmt.Errorf("exactly one of -upgrade-builder-name, -upgrade-build-id, or -upgrade-fuchsia-build-dir must be specified")
	}

	if c.upgradeBuilderName == "" {
		return fmt.Errorf("-upgrade-builder-name to be specified")
	}

	if c.cycleCount < 1 {
		return fmt.Errorf("-cycle-count must be >= 1")
	}

	return nil
}

func (c *config) shouldRepaveDevice() bool {
	return c.downgradeBuildID != "" || c.downgradeBuilderName != "" || c.downgradeFuchsiaBuildDir != ""
}

func (c *config) getDowngradeBuilder() (*artifacts.Builder, error) {
	if c.downgradeBuilderName == "" {
		return nil, fmt.Errorf("downgrade builder not specified")
	}

	return c.archiveConfig.BuildArchive().GetBuilder(c.downgradeBuilderName), nil
}

func (c *config) getDowngradeBuildID(ctx context.Context) (string, error) {
	if c.downgradeBuilderName != "" && c.downgradeBuildID == "" {
		b, err := c.getDowngradeBuilder()
		if err != nil {
			return "", err
		}
		id, err := b.GetLatestBuildID(ctx)
		if err != nil {
			return "", fmt.Errorf("failed to lookup build id: %w", err)
		}
		c.downgradeBuildID = id
	}

	return c.downgradeBuildID, nil
}

func (c *config) getDowngradeBuild(ctx context.Context, dir string) (artifacts.Build, error) {
	sshPrivateKey, err := c.deviceConfig.SSHPrivateKey()
	if err != nil {
		return nil, fmt.Errorf("failed to get ssh key: %w", err)
	}

	buildID, err := c.getDowngradeBuildID(ctx)
	if err != nil {
		return nil, err
	}

	if buildID != "" {
		return c.archiveConfig.BuildArchive().GetBuildByID(ctx, buildID, dir, sshPrivateKey.PublicKey())
	}

	if c.downgradeFuchsiaBuildDir != "" {
		return artifacts.NewFuchsiaDirBuild(c.downgradeFuchsiaBuildDir, sshPrivateKey.PublicKey()), nil
	}

	// the downgrade build isn't required, so don't err if we can't create one.
	return nil, nil
}

func (c *config) getUpgradeBuilder() (*artifacts.Builder, error) {
	if c.upgradeBuilderName == "" {
		return nil, fmt.Errorf("upgrade builder not specified")
	}

	return c.archiveConfig.BuildArchive().GetBuilder(c.upgradeBuilderName), nil
}

func (c *config) getUpgradeBuildID(ctx context.Context) (string, error) {
	if c.upgradeBuilderName != "" && c.upgradeBuildID == "" {
		b, err := c.getUpgradeBuilder()
		if err != nil {
			return "", err
		}
		id, err := b.GetLatestBuildID(ctx)
		if err != nil {
			return "", fmt.Errorf("failt to lookup build id: %w", err)
		}
		c.upgradeBuildID = id
	}

	return c.upgradeBuildID, nil
}
