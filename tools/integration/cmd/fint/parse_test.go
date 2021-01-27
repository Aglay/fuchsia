// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package main

import (
	"testing"
)

func TestParseStaticProto(t *testing.T) {
	textproto := `optimize: RELEASE
board: "qemu"
product: "workstation"
exclude_images: false
ninja_targets: "default"
include_host_tests: false
target_arch: X64
include_archives: false
skip_if_unaffected: true
`

	if _, err := parseStatic(textproto); err != nil {
		t.Errorf("failed to parse static .textproto: %s", err)
	}
}

func TestParseContextProto(t *testing.T) {
	textproto := `checkout_dir: "/a/b/c/fuchsia/"
build_dir: "/a/b/c/fuchsia/out/release"
artifact_dir: "/a/b/c/fuchsia/out/artifacts"
`

	if _, err := parseContext(textproto); err != nil {
		t.Errorf("failed to parse context .textproto: %s", err)
	}
}
