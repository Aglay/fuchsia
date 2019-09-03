// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Package testrunner handles specifics related to the testrunner tool.
package testrunner

import (
	"time"

	"go.fuchsia.dev/tools/testing/runtests"
)

// TestResult is the result of executing a test.
type TestResult struct {
	// Name is the name of the test that was executed.
	Name string

	// Result describes whether the test passed or failed.
	Result runtests.TestResult

	Stdout    []byte
	Stderr    []byte
	StartTime time.Time
	EndTime   time.Time
}
