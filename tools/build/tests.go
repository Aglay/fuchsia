// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can
// found in the LICENSE file.

package build

import (
	"encoding/json"
	"fmt"
	"os"
)

// TestSpec is the specification for a single test and the environments it
// should be executed in.
type TestSpec struct {
	// Test is the test that this specification is for.
	Test `json:"test"`

	// Envs is a set of environments that the test should be executed in.
	Envs []Environment `json:"environments"`
}

// Test encapsulates details about a particular test.
type Test struct {
	// Name is the "basename" of the test, e.g. "foo_test".
	Name string `json:"name"`

	// PackageURL is the fuchsia package URL for this test. It is only set for
	// tests targeting Fuchsia.
	PackageURL string `json:"package_url,omitempty"`

	// Path is the path to the test on the target OS.
	Path string `json:"path"`

	// Label is the full GN label with toolchain for the test target.
	// E.g.: //src/foo/tests:foo_tests(//build/toolchain/fuchsia:x64)
	Label string `json:"label"`

	// OS is the operating system in which this test must be executed.
	OS string `json:"os"`

	// Command is the command line to run to execute this test.
	Command []string `json:"command,omitempty"`

	// RuntimeDepsFile is a relative path within the build directory to a file
	// containing a JSON list of the test's runtime dependencies, Currently this
	// field only makes sense for Linux and Mac tests.
	RuntimeDepsFile string `json:"runtime_deps,omitempty"`

	// TODO(fxbug.dev/37955): Have this instead as a top-level target in
	// testsharder.Shard.
	// Deps is the list of runtime dependencies (on the host) of the test.
	Deps []string `json:"deps,omitempty"`
}

// Environment describes the full environment a test requires.
// The GN environments specified by test authors in the Fuchsia source
// correspond directly to the Environment struct defined here.
type Environment struct {
	// Dimensions gives the Swarming dimensions a test wishes to target.
	Dimensions DimensionSet `json:"dimensions"`

	// Tags are keys given to an environment on which the testsharder may filter.
	Tags []string `json:"tags,omitempty"`

	// ServiceAccount gives a service account to attach to Swarming task.
	ServiceAccount string `json:"service_account,omitempty"`

	// Netboot tells whether to "netboot" instead of paving before running the tests.
	Netboot bool `json:"netboot,omitempty"`
}

// DimensionSet encapsulates the Swarming dimensions a test wishes to target.
type DimensionSet struct {
	// DeviceType represents the class of device the test should run on.
	// This is a required field.
	DeviceType string `json:"device_type,omitempty"`

	// The OS to run the test on (e.g., "Linux" or "Mac"). Used for host-side testing.
	OS string `json:"os,omitempty"`

	// The CPU type that the test is meant to run on.
	CPU string `json:"cpu,omitempty"`

	// Testbed denotes a physical test device configuration to run a test on (e.g., multi-device set-ups or devices inside chambers for connectivity testing).
	Testbed string `json:"testbed,omitempty"`

	// Pool denotes the swarming pool to run a test in.
	Pool string `json:"pool,omitempty"`
}

// LoadTestSpecs reads in the entries in a given test manifest.
func LoadTestSpecs(manifest string) ([]TestSpec, error) {
	f, err := os.Open(manifest)
	if err != nil {
		return nil, fmt.Errorf("failed to open %s: %v", manifest, err)
	}
	defer f.Close()
	var specs []TestSpec
	if err := json.NewDecoder(f).Decode(&specs); err != nil {
		return nil, fmt.Errorf("failed to decode %s: %v", manifest, err)
	}
	return specs, nil
}

// LoadPlatforms reads in the entries in a given platform manifest.
func LoadPlatforms(manifest string) ([]DimensionSet, error) {
	f, err := os.Open(manifest)
	if err != nil {
		return nil, fmt.Errorf("failed to open %s: %v", manifest, err)
	}
	defer f.Close()
	var platforms []DimensionSet
	if err := json.NewDecoder(f).Decode(&platforms); err != nil {
		return nil, fmt.Errorf("failed to decode %s: %v", manifest, err)
	}
	return platforms, nil
}
