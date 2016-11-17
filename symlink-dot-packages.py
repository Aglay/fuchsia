#!/usr/bin/env python
# Copyright 2016 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import argparse
import json
import os
import paths
import stat
import subprocess
import sys


def gn_desc(outdir):
    '''Run `gn desc` over the whole tree'''
    return json.loads(subprocess.check_output(
        [os.path.join(paths.FUCHSIA_ROOT, 'packages', 'gn', 'gn.py'), 'desc',
         outdir, '*', '--format=json']))


def main():
    parser = argparse.ArgumentParser(
        description=
        'Symlink generated .packages to source directories to help IDEs')
    parser.add_argument("--arch", "-a",
                        choices=["x86-64", "arm64"],
                        default="x86-64",
                        help="architecture (default: x86-64)")
    parser.add_argument("--debug", "-d",
                        dest="variant",
                        action="store_const",
                        const="debug",
                        default="debug",
                        help="use debug build")
    parser.add_argument("--release", "-r",
                        dest="variant",
                        action="store_const",
                        const="release",
                        help="use release build")
    args = parser.parse_args()

    build_type = '%s-%s' % (args.variant, args.arch)
    outdir = os.path.join(paths.FUCHSIA_ROOT, 'out', build_type)
    if not os.path.exists(outdir):
        print 'ERROR: %s does not exist' % outdir
        sys.exit(2)
    success = True

    for target, props in gn_desc(outdir).items():
        # Only look at scripts that run the Dart or Flutter snapshotters
        if props.get('type') != 'action': continue
        if not props.get('script') in {
            '//flutter/build/snapshot.py', '//out/%s/host_x64/dart_snapshotter'
            % build_type
        }:
            continue

        # Look for the --packages args to the snapshotters
        args = props['args']
        if '--packages' in args:
            packages = args[args.index('--packages') + 1]
        else:
            for arg in args:
                if arg.startswith('--packages='):
                    packages = arg[len('--packages='):]
                    break
            else:
                raise RuntimeError('--packages= not found in %r for %s' %
                                   (args, target))

        # work out where the target is actually located
        target_dir = os.path.join(paths.FUCHSIA_ROOT, target.split(':')[0][2:])

        # where should the .packages symlink go
        symlink = os.path.join(target_dir, '.packages')

        if not os.path.exists(packages):
            print 'ERROR:    %s not found.' % packages
            success = False
            continue

        if os.path.exists(symlink):
            if stat.S_ISLNK(os.lstat(symlink).st_mode):
                if os.readlink(symlink) != packages:
                    print 'UPDATING: %s' % symlink
                    os.unlink(symlink)
                    os.symlink(packages, symlink)
                else:
                    print 'OK:       %s' % symlink
            else:
                print 'IGNORING: %s' % symlink
        else:
            print 'LINKING:  %s' % symlink
            os.symlink(packages, symlink)
    if not success:
        sys.exit(1)


if __name__ == '__main__':
    main()
