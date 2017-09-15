#!/usr/bin/env python
# Copyright 2016 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# Top-level build script for ThinFS.
# Build ThinFS targetting Fuchsia.

import argparse
import os
import sys
from subprocess import call


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--fuchsia-root', help='Path to root of Fuchsia project',
                        required=True)
    parser.add_argument('--go-binary', help='Path to go binary which can compile Fuchsia programs',
                        required=True)
    parser.add_argument('--go-bin-directory', help='Path to GOBIN',
                        required=True)
    args = parser.parse_args()

    scripts_dir = os.path.dirname(os.path.realpath(__file__))
    os.environ['CGO_ENABLED'] = '1'
    os.environ['GOROOT'] = os.path.join(args.fuchsia_root, 'third_party/go')
    os.environ['GOPATH'] = os.path.join(args.fuchsia_root, 'go')
    # TODO(smklein): Remove this usage of gccwrap once we've converted to building
    # the go runtime with clangwrap.
    os.environ['MAGENTA'] = os.path.join(args.fuchsia_root, 'magenta')
    os.environ['CC'] = os.path.join(args.fuchsia_root, 'third_party/go/misc/fuchsia/gccwrap.sh')
    os.environ['GOOS'] = 'fuchsia'
    os.environ['GOBIN'] = args.go_bin_directory
    target = os.path.join(scripts_dir, '../magenta/thinfs.go')
    return call([args.go_binary, 'install', target], env=os.environ)


if __name__ == '__main__':
    main()
