// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package main

import (
	"archive/tar"
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"

	"go.fuchsia.dev/fuchsia/tools/lib/osmisc"
	"go.fuchsia.dev/fuchsia/tools/lib/tarutil"
	"go.fuchsia.dev/fuchsia/tools/testing/runtests"
	"go.fuchsia.dev/fuchsia/tools/testing/tap/lib"
	"go.fuchsia.dev/fuchsia/tools/testing/testrunner/lib"
)

// testOutput manages the test runner's output drivers. Upon completion, if tar output is
// initialized, a TAR archive containing all other outputs is produced.
type testOutputs struct {
	dataDir string
	summary runtests.TestSummary
	tap     *tap.Producer
	tw      *tar.Writer
	outDir  string
}

func createTestOutputs(producer *tap.Producer, dataDir, archivePath, outDir string) (*testOutputs, error) {
	var tw *tar.Writer
	if archivePath != "" {
		f, err := os.Create(archivePath)
		if err != nil {
			return nil, fmt.Errorf("failed to create file %q: %v", archivePath, err)
		}
		tw = tar.NewWriter(f)
	}

	return &testOutputs{
		dataDir: dataDir,
		tap:     producer,
		tw:      tw,
		outDir:  outDir,
	}, nil
}

// Record writes the test result to initialized outputs.
func (o *testOutputs) record(result testrunner.TestResult) error {
	outputRelPath := filepath.Join(result.Name, runtests.TestOutputFilename)
	// Strip any leading //.
	outputRelPath = strings.TrimLeft(outputRelPath, "//")

	duration := result.EndTime.Sub(result.StartTime)
	if duration <= 0 {
		return fmt.Errorf("test %q must have positive duration: (start, end) = (%v, %v)", result.Name, result.StartTime, result.EndTime)
	}

	o.summary.Tests = append(o.summary.Tests, runtests.TestDetails{
		Name:       result.Name,
		GNLabel:    result.GNLabel,
		OutputFile: outputRelPath,
		Result:     result.Result,
		StartTime:  result.StartTime,
		// TODO(fxbug.dev/43518): when 1.13 is available, spell this as `duration.Milliseconds()`.
		DurationMillis: duration.Nanoseconds() / (1000 * 1000),
		DataSinks:      result.DataSinks,
	})

	desc := fmt.Sprintf("%s (%v)", result.Name, duration)
	o.tap.Ok(result.Result == runtests.TestSuccess, desc)

	var err error
	// TODO(fxb/43500): Remove once we've switched to using outDir.
	if o.tw != nil {
		stdout := bytes.NewReader(result.Stdout)
		stderr := bytes.NewReader(result.Stderr)
		stdio := io.MultiReader(stdout, stderr)
		size := stdout.Size() + stderr.Size()
		err = tarutil.TarFromReader(o.tw, stdio, outputRelPath, size)
	}
	if o.outDir != "" {
		stdout := bytes.NewReader(result.Stdout)
		stderr := bytes.NewReader(result.Stderr)
		stdio := io.MultiReader(stdout, stderr)
		outputRelPath = filepath.Join(o.outDir, outputRelPath)
		pathWriter, err := osmisc.CreateFile(outputRelPath)
		if err != nil {
			return fmt.Errorf("failed to create file: %v", err)
		}
		defer pathWriter.Close()
		_, err = io.Copy(pathWriter, stdio)
	}
	if err != nil {
		return fmt.Errorf("failed to write stdio file for test %q: %v", result.Name, err)
	}

	if o.tw != nil || o.outDir != "" {
		for _, sinks := range result.DataSinks {
			for _, sink := range sinks {
				sinkSrc := filepath.Join(o.dataDir, sink.File)
				// TODO(fxb/43500): Remove once we've switched to outDir.
				if o.tw != nil {
					if err := tarutil.TarFile(o.tw, sinkSrc, sink.File); err != nil {
						return fmt.Errorf("failed to tar data sink %q: %v", sink.Name, err)
					}
				}
				if o.outDir != "" {
					srcReader, err := os.Open(sinkSrc)
					if err != nil {
						return fmt.Errorf("failed to open sink src %q: %v", sink.Name, err)
					}
					defer srcReader.Close()
					dest := filepath.Join(o.outDir, sink.File)
					destWriter, err := osmisc.CreateFile(dest)
					if err != nil {
						return fmt.Errorf("failed to create file: %v", err)
					}
					defer destWriter.Close()
					if _, err := io.Copy(destWriter, srcReader); err != nil {
						return fmt.Errorf("failed to copy data sink %q to out dir: %v", sink.Name, err)
					}
				}
			}
		}
	}
	return nil
}

// Close stops the recording of test outputs; it must be called to finalize them.
func (o *testOutputs) Close() error {
	if o.tw == nil && o.outDir == "" {
		return nil
	}
	summaryBytes, err := json.Marshal(o.summary)
	if err != nil {
		return err
	}
	if o.tw != nil {
		if err := tarutil.TarBytes(o.tw, summaryBytes, runtests.TestSummaryFilename); err != nil {
			return err
		}
		err = o.tw.Close()
	}
	if o.outDir != "" {
		summaryPath := filepath.Join(o.outDir, runtests.TestSummaryFilename)
		s, err := osmisc.CreateFile(summaryPath)
		if err != nil {
			return fmt.Errorf("failed to create file: %v", err)
		}
		defer s.Close()
		_, err = io.Copy(s, bytes.NewBuffer(summaryBytes))
	}
	return err
}
