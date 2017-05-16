#!/usr/bin/env python
# Copyright 2017 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# Build script for a Go app.

import argparse
import os
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--fuchsia-root', help='Path to root of Fuchsia project',
                        required=True)
    parser.add_argument('--root-out-dir', help='Path to root of build output',
                        required=True)
    parser.add_argument('--depfile', help='The path to the depfile',
                        required=True)
    parser.add_argument('--current-cpu', help='Target arch, x64 or arm64.',
                        required=True)
    parser.add_argument('--go-tool', help='The go tool to use for builds')
    parser.add_argument('--is-test', help='True if the target is a go test',
                        default=False)
    parser.add_argument('--binname', help='Output file')
    parser.add_argument('package', help='The package name')
    args = parser.parse_args()

    if args.current_cpu == "x64":
        goarch = "amd64"
    elif args.current_cpu == "arm64":
        goarch = "arm64"
    else:
        print("unknown current_cpu: ", args.current_cpu)
        return 1

    gopath = args.root_out_dir

    os.environ['CGO_ENABLED'] = '1'
    os.environ['GOPATH'] = gopath + ":" + os.path.join(args.root_out_dir, "gen/go")
    os.environ['GOOS'] = 'fuchsia'
    os.environ['GOARCH'] = goarch
    if 'GOBIN' in os.environ:
        del os.environ['GOBIN']
    if 'GOROOT' in os.environ:
        del os.environ['GOROOT']
    godepfile = os.path.join(args.fuchsia_root, 'buildtools/godepfile')

    if args.is_test:
        retcode = subprocess.call([args.go_tool, 'test', '-c', '-o', args.binname,
                                  args.package], env=os.environ)
    else:
        retcode = subprocess.call([args.go_tool, 'install', args.package],
                                  env=os.environ)

    if retcode == 0:
        if args.is_test:
            binname = args.binname
        else:
            # For regular Go binaries, they are placed in a "bin/fuchsia_ARCH"
            # output directory; this relocates them to the root of the
            # gopath. Tests are not impacted.
            binname = os.path.basename(args.package)
            src = os.path.join(gopath, "bin", "fuchsia_"+goarch, binname)
            if args.binname:
                dst = os.path.join(gopath, args.binname)
            else:
                dst = os.path.join(gopath, binname)
            os.rename(src, dst)

        if args.depfile is not None:
            with open(args.depfile, "wb") as out:
                os.environ['GOROOT'] = os.path.join(args.fuchsia_root, "third_party/go")
                subprocess.Popen([godepfile, '-o', binname, args.package], stdout=out, env=os.environ)
    return retcode


if __name__ == '__main__':
    sys.exit(main())
