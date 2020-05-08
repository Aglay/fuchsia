// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package main

import (
	"context"
	"fmt"
	"io"
	"testing"

	"go.fuchsia.dev/fuchsia/tools/build/lib"
	"go.fuchsia.dev/fuchsia/tools/integration/testsharder/lib"
	"go.fuchsia.dev/fuchsia/tools/net/sshutil"
	"go.fuchsia.dev/fuchsia/tools/testing/runtests"
	"go.fuchsia.dev/fuchsia/tools/testing/testrunner/lib"
)

type fakeTester struct {
	testErr error
}

func (t *fakeTester) Test(ctx context.Context, test testsharder.Test, stdout, stderr io.Writer) (runtests.DataSinkReference, error) {
	return nil, t.testErr
}

func (t *fakeTester) Close() error {
	return nil
}

func (t *fakeTester) CopySinks(ctx context.Context, sinks []runtests.DataSinkReference) error {
	return nil
}

func assertEqual(t1, t2 *testrunner.TestResult) bool {
	return t1.Name == t2.Name && t1.Result == t2.Result && t1.RunIndex == t2.RunIndex
}

func TestRunTest(t *testing.T) {
	cases := []struct {
		name           string
		test           build.Test
		runs           int
		testErr        error
		expectedErr    error
		expectedResult []*testrunner.TestResult
	}{
		{
			name: "host test pass",
			test: build.Test{
				Name: "bar",
				Path: "/foo/bar",
				OS:   "linux",
			},
			testErr: nil,
			expectedResult: []*testrunner.TestResult{{
				Name:   "/foo/bar",
				Result: runtests.TestSuccess,
			}},
		},
		{
			name: "fuchsia test pass",
			test: build.Test{
				Name:       "bar",
				Path:       "/foo/bar",
				OS:         "fuchsia",
				PackageURL: "fuchsia-pkg://foo/bar",
			},
			testErr: nil,
			expectedResult: []*testrunner.TestResult{{
				Name:   "fuchsia-pkg://foo/bar",
				Result: runtests.TestSuccess,
			}},
		},
		{
			name: "fuchsia test fail",
			test: build.Test{
				Name:       "bar",
				Path:       "/foo/bar",
				OS:         "fuchsia",
				PackageURL: "fuchsia-pkg://foo/bar",
			},
			testErr: fmt.Errorf("test failed"),
			expectedResult: []*testrunner.TestResult{{
				Name:   "fuchsia-pkg://foo/bar",
				Result: runtests.TestFailure,
			}},
		},
		{
			name: "fuchsia test ssh connection fail",
			test: build.Test{
				Name:       "bar",
				Path:       "/foo/bar",
				OS:         "fuchsia",
				PackageURL: "fuchsia-pkg://foo/bar",
			},
			testErr:        sshutil.ConnectionError,
			expectedErr:    sshutil.ConnectionError,
			expectedResult: nil,
		},
		{
			name: "multiplier test gets unique index",
			test: build.Test{
				Name:       "bar (2)",
				Path:       "/foo/bar",
				OS:         "fuchsia",
				PackageURL: "fuchsia-pkg://foo/bar",
			},
			runs:    2,
			testErr: nil,
			expectedResult: []*testrunner.TestResult{{
				Name:   "fuchsia-pkg://foo/bar",
				Result: runtests.TestSuccess,
			}, {
				Name:     "fuchsia-pkg://foo/bar",
				Result:   runtests.TestSuccess,
				RunIndex: 1,
			}},
		},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			tester := &fakeTester{
				testErr: c.testErr,
			}
			if c.runs == 0 {
				c.runs = 1
			}
			for i := 0; i < c.runs; i++ {
				result, err := runTest(context.Background(), testsharder.Test{c.test, c.runs}, i, tester)

				if err != c.expectedErr {
					t.Errorf("got error: %v, expected: %v", err, c.expectedErr)
				}
				if err == nil {
					if !assertEqual(result, c.expectedResult[i]) {
						t.Errorf("got result: %v, expected: %v", result, c.expectedResult[i])
					}
				}
			}
		})
	}
}
