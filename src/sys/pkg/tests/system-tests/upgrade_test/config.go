// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package upgrade

import (
	"flag"
	"fmt"
	"io/ioutil"
	"os"
	"path/filepath"
	"strings"
	"time"

	"fuchsia.googlesource.com/host_target_testing/artifacts"
	"fuchsia.googlesource.com/host_target_testing/device"
	"fuchsia.googlesource.com/host_target_testing/packages"
	"fuchsia.googlesource.com/host_target_testing/paver"
	"fuchsia.googlesource.com/host_target_testing/util"

	"golang.org/x/crypto/ssh"
)

type Config struct {
	outputDir                string
	FuchsiaDir               string
	sshKeyFile               string
	netaddrPath              string
	DeviceName               string
	deviceHostname           string
	LkgbPath                 string
	ArtifactsPath            string
	PackagesPath             string
	downgradeBuilderName     string
	downgradeBuildID         string
	downgradeFuchsiaBuildDir string
	upgradeBuilderName       string
	upgradeBuildID           string
	upgradeFuchsiaBuildDir   string
	LongevityTest            bool
	archive                  *artifacts.Archive
	sshPrivateKey            ssh.Signer
	paveTimeout              time.Duration
	cycleTimeout             time.Duration
}

func NewConfig(fs *flag.FlagSet) (*Config, error) {
	c := &Config{}

	testDataPath := filepath.Join(filepath.Dir(os.Args[0]), "test_data", "system-tests")

	fs.StringVar(&c.outputDir, "output-dir", "", "save temporary files to this directory, defaults to a tempdir")
	fs.StringVar(&c.FuchsiaDir, "fuchsia-dir", os.Getenv("FUCHSIA_DIR"), "fuchsia dir")
	fs.StringVar(&c.sshKeyFile, "ssh-private-key", os.Getenv("FUCHSIA_SSH_KEY"), "SSH private key file that can access the device")
	fs.StringVar(&c.netaddrPath, "netaddr-path", filepath.Join(testDataPath, "netaddr"), "zircon netaddr tool path")
	fs.StringVar(&c.DeviceName, "device", os.Getenv("FUCHSIA_NODENAME"), "device name")
	fs.StringVar(&c.deviceHostname, "device-hostname", os.Getenv("FUCHSIA_IPV4_ADDR"), "device hostname or IPv4/IPv6 address")
	fs.StringVar(&c.LkgbPath, "lkgb", filepath.Join(testDataPath, "lkgb"), "path to lkgb, default is $FUCHSIA_DIR/prebuilt/tools/lkgb/lkgb")
	fs.StringVar(&c.ArtifactsPath, "artifacts", filepath.Join(testDataPath, "artifacts"), "path to the artifacts binary, default is $FUCHSIA_DIR/prebuilt/tools/artifacts/artifacts")
	fs.StringVar(&c.downgradeBuilderName, "downgrade-builder-name", "", "downgrade to the latest version of this builder")
	fs.StringVar(&c.downgradeBuildID, "downgrade-build-id", "", "downgrade to this specific build id")
	fs.StringVar(&c.downgradeFuchsiaBuildDir, "downgrade-fuchsia-build-dir", "", "Path to the downgrade fuchsia build dir")
	fs.StringVar(&c.upgradeBuilderName, "upgrade-builder-name", "", "upgrade to the latest version of this builder")
	fs.StringVar(&c.upgradeBuildID, "upgrade-build-id", os.Getenv("BUILDBUCKET_ID"), "upgrade to this build id (default is $BUILDBUCKET_ID)")
	fs.StringVar(&c.upgradeFuchsiaBuildDir, "upgrade-fuchsia-build-dir", "", "Path to the upgrade fuchsia build dir")
	fs.BoolVar(&c.LongevityTest, "longevity-test", false, "Continuously update to the latest repository")
	fs.DurationVar(&c.paveTimeout, "pave-timeout", 1<<63-1, "Err if a pave it takes longer than this time (default is no timeout)")
	fs.DurationVar(&c.cycleTimeout, "cycle-timeout", 1<<63-1, "Err if a test cycle it takes longer than this time (default is no timeout)")

	return c, nil
}

func (c *Config) Validate() error {
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

	if c.LongevityTest && c.upgradeBuilderName == "" {
		return fmt.Errorf("-longevity-test requires -upgrade-builder-name to be specified")
	}

	return nil
}

func (c *Config) OutputDir() (string, func(), error) {
	// If we specified an -output-dir, return it, and a cleanup function
	// that does nothoing.
	if c.outputDir != "" {
		return c.outputDir, func() {}, nil
	}

	// Otherwise create a tempdir, and return a cleanup function that
	// deletes the tempdir when called.
	outputDir, err := ioutil.TempDir("", "system_ota_tests")
	if err != nil {
		return "", func() {}, err
	}

	return outputDir, func() { os.RemoveAll(outputDir) }, nil
}

func (c *Config) SshPrivateKey() (ssh.Signer, error) {
	if c.sshPrivateKey == nil {
		if c.sshKeyFile == "" {
			return nil, fmt.Errorf("ssh private key cannot be empty")
		}

		key, err := ioutil.ReadFile(c.sshKeyFile)
		if err != nil {
			return nil, err
		}

		privateKey, err := ssh.ParsePrivateKey(key)
		if err != nil {
			return nil, err
		}
		c.sshPrivateKey = privateKey
	}

	return c.sshPrivateKey, nil
}

func (c *Config) NewDeviceClient() (*device.Client, error) {
	deviceHostname, err := c.DeviceHostname()
	if err != nil {
		return nil, err
	}

	sshPrivateKey, err := c.SshPrivateKey()
	if err != nil {
		return nil, err
	}

	return device.NewClient(deviceHostname, sshPrivateKey)
}

func (c *Config) BuildArchive() *artifacts.Archive {
	if c.archive == nil {
		// Connect to the build archive service.
		c.archive = artifacts.NewArchive(c.LkgbPath, c.ArtifactsPath)
	}

	return c.archive
}

func (c *Config) ShouldRepaveDevice() bool {
	return c.downgradeBuildID != "" || c.downgradeBuilderName != "" || c.downgradeFuchsiaBuildDir != ""
}

func (c *Config) GetDowngradeBuilder() (*artifacts.Builder, error) {
	if c.downgradeBuilderName == "" {
		return nil, fmt.Errorf("downgrade builder not specified")
	}

	return c.BuildArchive().GetBuilder(c.downgradeBuilderName), nil
}

func (c *Config) GetDowngradeBuildID() (string, error) {
	if c.downgradeBuilderName != "" && c.downgradeBuildID == "" {
		b, err := c.GetDowngradeBuilder()
		if err != nil {
			return "", err
		}
		id, err := b.GetLatestBuildID()
		if err != nil {
			return "", fmt.Errorf("failed to lookup build id: %s", err)
		}
		c.downgradeBuildID = id
	}

	return c.downgradeBuildID, nil
}

func (c *Config) GetUpgradeBuilder() (*artifacts.Builder, error) {
	if c.upgradeBuilderName == "" {
		return nil, fmt.Errorf("upgrade builder not specified")
	}

	return c.BuildArchive().GetBuilder(c.upgradeBuilderName), nil
}

func (c *Config) GetUpgradeBuildID() (string, error) {
	if c.upgradeBuilderName != "" && c.upgradeBuildID == "" {
		b, err := c.GetUpgradeBuilder()
		if err != nil {
			return "", err
		}
		id, err := b.GetLatestBuildID()
		if err != nil {
			return "", fmt.Errorf("failt to lookup build id: %s", err)
		}
		c.upgradeBuildID = id
	}

	return c.upgradeBuildID, nil
}

func (c *Config) GetDowngradeRepository(dir string) (*packages.Repository, error) {
	buildID, err := c.GetDowngradeBuildID()
	if err != nil {
		return nil, err
	}

	if buildID != "" {
		build, err := c.BuildArchive().GetBuildByID(buildID, dir)
		if err != nil {
			return nil, err
		}

		return build.GetPackageRepository()
	}

	if c.downgradeFuchsiaBuildDir != "" {
		return packages.NewRepository(filepath.Join(c.downgradeFuchsiaBuildDir, "amber-files"))
	}

	return nil, fmt.Errorf("downgrade repository not specified")
}

func (c *Config) GetUpgradeRepository(dir string) (*packages.Repository, error) {
	buildID, err := c.GetUpgradeBuildID()
	if err != nil {
		return nil, err
	}

	if buildID != "" {
		build, err := c.BuildArchive().GetBuildByID(buildID, dir)
		if err != nil {
			return nil, err
		}

		return build.GetPackageRepository()
	}

	if c.upgradeFuchsiaBuildDir != "" {
		return packages.NewRepository(filepath.Join(c.upgradeFuchsiaBuildDir, "amber-files"))
	}

	return nil, fmt.Errorf("upgrade repository not specified")
}

func (c *Config) GetDowngradePaver(dir string) (*paver.Paver, error) {
	sshPrivateKey, err := c.SshPrivateKey()
	if err != nil {
		return nil, err
	}
	sshPublicKey := sshPrivateKey.PublicKey()

	buildID, err := c.GetDowngradeBuildID()
	if err != nil {
		return nil, err
	}

	if buildID != "" {
		build, err := c.BuildArchive().GetBuildByID(buildID, dir)
		if err != nil {
			return nil, err
		}

		return build.GetPaver(sshPublicKey)
	}

	if c.downgradeFuchsiaBuildDir != "" {
		return paver.NewPaver(
				filepath.Join(c.downgradeFuchsiaBuildDir, "pave-zedboot.sh"),
				filepath.Join(c.downgradeFuchsiaBuildDir, "pave.sh"),
				sshPublicKey),
			nil
	}

	return nil, fmt.Errorf("downgrade paver not specified")
}

func (c *Config) DeviceHostname() (string, error) {
	if c.deviceHostname == "" {
		var err error
		c.deviceHostname, err = c.netaddr("--nowait", "--timeout=1000", "--fuchsia", c.DeviceName)
		if err != nil {
			return "", fmt.Errorf("ERROR: netaddr failed: %s", err)
		}
		if c.deviceHostname == "" {
			return "", fmt.Errorf("unable to determine the device hostname")
		}
	}

	return c.deviceHostname, nil
}

func (c *Config) netaddr(arg ...string) (string, error) {
	stdout, stderr, err := util.RunCommand(c.netaddrPath, arg...)
	if err != nil {
		return "", fmt.Errorf("netaddr failed: %s: %s", err, string(stderr))
	}
	return strings.TrimRight(string(stdout), "\n"), nil
}
