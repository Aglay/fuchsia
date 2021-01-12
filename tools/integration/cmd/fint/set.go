// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package main

import (
	"bytes"
	"context"
	"flag"
	"fmt"
	"io"
	"io/ioutil"
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"strings"

	"github.com/golang/protobuf/proto"
	"github.com/google/subcommands"
	fintpb "go.fuchsia.dev/fuchsia/tools/integration/cmd/fint/proto"
	"go.fuchsia.dev/fuchsia/tools/lib/logger"
	"go.fuchsia.dev/fuchsia/tools/lib/osmisc"
	"go.fuchsia.dev/fuchsia/tools/lib/runner"
)

const (
	fuchsiaDirEnvVar = "FUCHSIA_DIR"

	// Locations of GN trace files in the build directory.
	fuchsiaGNTrace = "gn_trace.json"

	// Name of the file (in `contextSpec.ArtifactsDir`) that will expose
	// manifest files and other metadata produced by this command to the caller.
	artifactsManifest = "set_artifacts.textproto"
)

type subprocessRunner interface {
	Run(ctx context.Context, cmd []string, stdout, stderr io.Writer) error
}

type SetCommand struct {
	staticSpecPath     string
	contextSpecPath    string
	failureSummaryPath string
}

func (*SetCommand) Name() string { return "set" }

func (*SetCommand) Synopsis() string { return "runs gn gen with args based on the input specs." }

func (*SetCommand) Usage() string {
	return `fint set -static <path> [-context <path>]

flags:
`
}

func (c *SetCommand) SetFlags(f *flag.FlagSet) {
	f.StringVar(&c.staticSpecPath, "static", "", "path to a Static .textproto file.")
	f.StringVar(
		&c.contextSpecPath,
		"context",
		"",
		("path to a Context .textproto file. If unset, the " +
			fuchsiaDirEnvVar +
			" will be used to locate the checkout."),
	)
	// TODO(fxbug.dev/65899): Delete this flag after recipes retrieve failure
	// summary via set artifacts instead.
	f.StringVar(
		&c.failureSummaryPath,
		"failure-summary",
		"",
		("if set, brief plain text logs that are useful for debugging in case of failures will " +
			"be written to this file ."),
	)
}

func (c *SetCommand) Execute(ctx context.Context, _ *flag.FlagSet, _ ...interface{}) subcommands.ExitStatus {
	if c.staticSpecPath == "" {
		logger.Errorf(ctx, "-static flag is required")
		return subcommands.ExitUsageError
	}
	if err := c.run(ctx); err != nil {
		logger.Errorf(ctx, err.Error())
		return subcommands.ExitFailure
	}
	return subcommands.ExitSuccess
}

func (c *SetCommand) run(ctx context.Context) error {
	bytes, err := ioutil.ReadFile(c.staticSpecPath)
	if err != nil {
		return err
	}

	staticSpec, err := parseStatic(string(bytes))
	if err != nil {
		return err
	}

	var contextSpec *fintpb.Context
	if c.contextSpecPath != "" {
		bytes, err = ioutil.ReadFile(c.contextSpecPath)
		if err != nil {
			return err
		}

		contextSpec, err = parseContext(string(bytes))
		if err != nil {
			return err
		}
	} else {
		// The -context flag should always be set in production, but fall back
		// to looking up the `fuchsiaDirEnvVar` to determine the checkout and
		// build directories to make fint less cumbersome to run manually.
		contextSpec, err = defaultContextSpec()
		if err != nil {
			return err
		}
	}

	platform, err := getPlatform()
	if err != nil {
		return err
	}

	runner := &runner.SubprocessRunner{}
	artifacts, runErr := runSteps(ctx, runner, staticSpec, contextSpec, platform)

	if c.failureSummaryPath != "" {
		f, err := osmisc.CreateFile(c.failureSummaryPath)
		if err != nil {
			return fmt.Errorf("failed to create failure summary file: %w", err)
		}
		defer f.Close()
		if _, err := f.WriteString(artifacts.FailureSummary); err != nil {
			return fmt.Errorf("failed to write failure summary: %w", err)
		}
	}

	if contextSpec.ArtifactDir != "" {
		f, err := osmisc.CreateFile(filepath.Join(contextSpec.ArtifactDir, artifactsManifest))
		if err != nil {
			return fmt.Errorf("failed to create artifacts file: %w", err)
		}
		defer f.Close()
		if err := proto.MarshalText(f, artifacts); err != nil {
			return fmt.Errorf("failed to write artifacts file: %w", err)
		}
	}

	return runErr
}

func defaultContextSpec() (*fintpb.Context, error) {
	checkoutDir, found := os.LookupEnv(fuchsiaDirEnvVar)
	if !found {
		return nil, fmt.Errorf("$%s must be set if -context is not set", fuchsiaDirEnvVar)
	}
	return &fintpb.Context{
		CheckoutDir: checkoutDir,
		BuildDir:    filepath.Join(checkoutDir, "out", "default"),
	}, nil
}

// runSteps runs `gn gen` along with any post-processing steps, and returns a
// SetArtifacts object containing metadata produced by GN and post-processing.
func runSteps(
	ctx context.Context,
	runner subprocessRunner,
	staticSpec *fintpb.Static,
	contextSpec *fintpb.Context,
	platform string,
) (*fintpb.SetArtifacts, error) {
	artifacts := &fintpb.SetArtifacts{}
	genArgs, err := genArgs(staticSpec, contextSpec, platform)
	if err != nil {
		return nil, err
	}
	genStdout, err := runGen(ctx, runner, staticSpec, contextSpec, platform, genArgs)
	if err != nil {
		artifacts.FailureSummary = genStdout
	}
	return artifacts, err
}

func runGen(
	ctx context.Context,
	runner subprocessRunner,
	staticSpec *fintpb.Static,
	contextSpec *fintpb.Context,
	platform string,
	args []string,
) (genStdout string, err error) {
	gnPath := filepath.Join(contextSpec.CheckoutDir, "prebuilt", "third_party", "gn", platform, "gn")
	genCmd := []string{
		gnPath, "gen",
		contextSpec.BuildDir,
		"--check=system",
		fmt.Sprintf("--tracelog=%s", filepath.Join(contextSpec.BuildDir, fuchsiaGNTrace)),
		"--fail-on-unused-args",
	}

	if staticSpec.GenerateCompdb {
		genCmd = append(genCmd, "--export-compile-commands")
	}
	if staticSpec.GenerateIde {
		genCmd = append(genCmd, "--ide=json")
	}

	genCmd = append(genCmd, fmt.Sprintf("--args=%s", strings.Join(args, " ")))

	// When `gn gen` fails, it outputs a brief helpful error message to stdout.
	var stdoutBuf bytes.Buffer
	if err := runner.Run(ctx, genCmd, io.MultiWriter(&stdoutBuf, os.Stdout), os.Stderr); err != nil {
		return stdoutBuf.String(), fmt.Errorf("error running gn gen: %w", err)
	}
	return stdoutBuf.String(), nil
}

func getPlatform() (string, error) {
	os, ok := map[string]string{
		"windows": "win",
		"darwin":  "mac",
		"linux":   "linux",
	}[runtime.GOOS]
	if !ok {
		return "", fmt.Errorf("unsupported GOOS %q", runtime.GOOS)
	}

	arch, ok := map[string]string{
		"amd64": "x64",
		"arm64": "arm64",
	}[runtime.GOARCH]
	if !ok {
		return "", fmt.Errorf("unsupported GOARCH %q", runtime.GOARCH)
	}
	return fmt.Sprintf("%s-%s", os, arch), nil
}

func genArgs(staticSpec *fintpb.Static, contextSpec *fintpb.Context, platform string) ([]string, error) {
	// GN variables to set via args (mapping from variable name to value).
	vars := make(map[string]interface{})
	// GN list variables to which we want to append via args (mapping from
	// variable name to list of values to append).
	appends := make(map[string][]string)
	// GN targets to import.
	var imports []string

	if staticSpec.TargetArch == fintpb.Static_ARCH_UNSPECIFIED {
		return nil, fmt.Errorf("target_arch is unspecified or invalid")
	}
	vars["target_cpu"] = strings.ToLower(staticSpec.TargetArch.String())

	if staticSpec.Optimize == fintpb.Static_OPTIMIZE_UNSPECIFIED {
		return nil, fmt.Errorf("optimize is unspecified or invalid")
	}
	vars["is_debug"] = staticSpec.Optimize == fintpb.Static_DEBUG

	if contextSpec.ClangToolchainDir != "" {
		if staticSpec.UseGoma {
			return nil, fmt.Errorf("goma is not supported for builds using a custom clang toolchain")
		}
		vars["clang_prefix"] = filepath.Join(contextSpec.ClangToolchainDir, "bin")
	}
	if contextSpec.GccToolchainDir != "" {
		if staticSpec.UseGoma {
			return nil, fmt.Errorf("goma is not supported for builds using a custom gcc toolchain")
		}
		vars["zircon_extra_args.gcc_tool_dir"] = filepath.Join(contextSpec.GccToolchainDir, "bin")
	}
	if contextSpec.RustToolchainDir != "" {
		vars["rustc_prefix"] = filepath.Join(contextSpec.RustToolchainDir, "bin")
	}

	vars["use_goma"] = staticSpec.UseGoma
	if staticSpec.UseGoma {
		vars["goma_dir"] = filepath.Join(
			contextSpec.CheckoutDir, "prebuilt", "third_party", "goma", platform,
		)
	}

	if staticSpec.Product != "" {
		basename := filepath.Base(staticSpec.Product)
		vars["build_info_product"] = strings.Split(basename, ".")[0]
		imports = append(imports, staticSpec.Product)
	}

	if staticSpec.Board != "" {
		basename := filepath.Base(staticSpec.Board)
		vars["build_info_board"] = strings.Split(basename, ".")[0]
		imports = append(imports, staticSpec.Board)
	}

	if contextSpec.SdkId != "" {
		vars["sdk_id"] = contextSpec.SdkId
		vars["build_sdk_archives"] = true
	}

	if contextSpec.ReleaseVersion != "" {
		vars["build_info_version"] = contextSpec.ReleaseVersion
	}

	if staticSpec.TestDurationsFile != "" {
		testDurationsFile := staticSpec.TestDurationsFile
		exists, err := osmisc.FileExists(filepath.Join(contextSpec.CheckoutDir, testDurationsFile))
		if err != nil {
			return nil, fmt.Errorf("failed to check if TestDurationsFile exists: %w", err)
		}
		if !exists {
			testDurationsFile = staticSpec.DefaultTestDurationsFile
		}
		vars["test_durations_file"] = testDurationsFile
	}

	for varName, packages := range map[string][]string{
		"base_package_labels":     staticSpec.BasePackages,
		"cache_package_labels":    staticSpec.CachePackages,
		"universe_package_labels": staticSpec.UniversePackages,
	} {
		if len(packages) == 0 {
			continue
		}
		// If product is set, append to the corresponding list variable instead
		// of overwriting it to avoid overwriting any packages set in the
		// imported product file.
		// TODO(olivernewman): Is it safe to always append whether or not
		// product is set?
		if staticSpec.Product == "" {
			vars[varName] = packages
		} else {
			appends[varName] = packages
		}
	}

	if len(staticSpec.Variants) != 0 {
		vars["select_variant"] = staticSpec.Variants
		if contains(staticSpec.Variants, "thinlto") {
			vars["thinlto_cache_dir"] = filepath.Join(contextSpec.CacheDir, "thinlto")
		}
	}

	var normalArgs []string
	var importArgs []string
	for _, arg := range staticSpec.GnArgs {
		if strings.HasPrefix(arg, "import(") {
			importArgs = append(importArgs, arg)
		} else {
			normalArgs = append(normalArgs, arg)
		}
	}

	for k, v := range vars {
		normalArgs = append(normalArgs, fmt.Sprintf("%s=%s", k, toGNValue(v)))
	}
	for k, v := range appends {
		normalArgs = append(normalArgs, fmt.Sprintf("%s+=%s", k, toGNValue(v)))
	}
	sort.Strings(normalArgs)

	for _, p := range imports {
		importArgs = append(importArgs, fmt.Sprintf(`import("//%s")`, p))
	}
	sort.Strings(importArgs)

	var finalArgs []string

	// Ensure that imports come before args that set or modify variables, as
	// otherwise the imported files might blindly redefine variables set or
	// modified by other arguments.
	finalArgs = append(finalArgs, importArgs...)
	// Initialize `zircon_extra_args` before any variable-setting args, so that
	// it's safe for subsequent args to do things like `zircon_extra_args.foo =
	// "bar"` without worrying about initializing zircon_extra_args if it hasn't
	// yet been defined. But do it after all imports in case one of the imported
	// files sets `zircon_extra_args`.
	finalArgs = append(finalArgs, "if (!defined(zircon_extra_args)) { zircon_extra_args = {} }")
	finalArgs = append(finalArgs, normalArgs...)
	return finalArgs, nil
}

// toGNValue converts a Go value to a string representation of the corresponding
// GN value by inspecting the Go value's type. This makes the logic to set GN
// args more readable and less error-prone, with no need for patterns like
// fmt.Sprintf(`"%s"`) repeated everywhere.
//
// For example:
// - toGNValue(true) => `true`
// - toGNValue("foo") => `"foo"` (a string containing literal double-quotes)
// - toGNValue([]string{"foo", "bar"}) => `["foo","bar"]`
func toGNValue(x interface{}) string {
	switch val := x.(type) {
	case bool:
		return fmt.Sprintf("%v", val)
	case string:
		// Apply double-quotes to strings, but not to GN scopes like
		// {variant="asan-fuzzer" target_type=["fuzzed_executable"]}
		if strings.HasPrefix(val, "{") && strings.HasSuffix(val, "}") {
			return val
		}
		return fmt.Sprintf(`"%s"`, val)
	case []string:
		var values []string
		for _, element := range val {
			values = append(values, toGNValue(element))
		}
		return fmt.Sprintf("[%s]", strings.Join(values, ","))
	default:
		panic(fmt.Sprintf("unsupported arg value type %T", val))
	}
}

func contains(items []string, target string) bool {
	for _, item := range items {
		if item == target {
			return true
		}
	}
	return false
}
