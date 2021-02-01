// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package support

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func ToolPath(t *testing.T, name string) string {
	ex, err := os.Executable()
	if err != nil {
		t.Fatal(err)
		return ""
	}
	exPath := filepath.Dir(ex)
	return filepath.Join(exPath, "test_data", "tools", name)
}

func EnsureContains(t *testing.T, output string, lookFor string) {
	if !strings.Contains(output, lookFor) {
		t.Fatalf("output did not contain '%s'", lookFor)
	}
}
