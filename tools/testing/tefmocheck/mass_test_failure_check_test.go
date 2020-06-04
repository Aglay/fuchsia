// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package tefmocheck

import (
	"testing"

	"go.fuchsia.dev/fuchsia/tools/testing/runtests"
)

func TestMassTestFailureCheck(t *testing.T) {
	const killerString = "KILLER STRING"
	c := MassTestFailureCheck{MaxFailed: 3}
	summary := runtests.TestSummary{
		Tests: []runtests.TestDetails{
			{Result: runtests.TestFailure},
			{Result: runtests.TestFailure},
			{Result: runtests.TestFailure},
		},
	}
	to := TestingOutputs{
		TestSummary: &summary,
	}
	if c.Check(&to) {
		t.Errorf("MassTestFailureCheck.Check() returned true with only 3 failed tests, expected false")
	}
	summary.Tests = append(summary.Tests, runtests.TestDetails{Result: runtests.TestFailure})
	if !c.Check(&to) {
		t.Errorf("MassTestFailureCheck.Check() returned false with 4 failed tests, expected true")
	}
}
