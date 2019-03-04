// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
package botanist

import (
	"io/ioutil"
	"os"
	"testing"
)

func TestLoadDevicePropertiesSlice(t *testing.T) {
	tests := []struct {
		name        string
		jsonStr     string
		expectedLen int
		expectErr   bool
	}{
		// Valid configs.
		{"ValidListConfig", `[{"nodename":"upper-drank-wick-creek"},{"nodename":"siren-swoop-wick-hasty"}]`, 2, false},
		{"ValidSingleConfig", `{"nodename":"upper-drank-wick-creek"}`, 1, false},
		// Invalid configs.
		{"InvalidListConfig", `{{"nodename":"upper-drank-wick-creek"},{"nodename":"siren-swoop-wick-hasty"}}`, 0, true},
		{"InvalidSingleConfig", `{"upper-drank-wick-creek"}`, 0, true},
	}
	for _, test := range tests {
		tmpfile, err := ioutil.TempFile(os.TempDir(), "common_test")
		if err != nil {
			t.Fatalf("Failed to create test device properties file: %s", err)
		}
		defer os.Remove(tmpfile.Name())

		content := []byte(test.jsonStr)
		if _, err := tmpfile.Write(content); err != nil {
			t.Fatalf("Failed to write to test device properties file: %s", err)
		}

		propertiesSlice, err := LoadDeviceProperties(tmpfile.Name())

		if test.expectErr && err == nil {
			t.Errorf("Test%v: Exepected errors; no errors found", test.name)
		}

		if !test.expectErr && err != nil {
			t.Errorf("Test%v: Exepected no errors; found error - %v", test.name, err)
		}

		if len(propertiesSlice) != test.expectedLen {
			t.Errorf("Test%v: Expected %d nodes; found %d", test.name, test.expectedLen, len(propertiesSlice))
		}

		if err := tmpfile.Close(); err != nil {
			t.Fatal(err)
		}
	}
}
