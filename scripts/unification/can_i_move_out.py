#!/usr/bin/env python2.7
# Copyright 2019 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import argparse
import os
import subprocess
import sys


SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
FUCHSIA_ROOT = os.path.dirname(  # $root
    os.path.dirname(             # scripts
    SCRIPT_DIR))                 # unification


class Finder(object):

    def __init__(self, gn_binary, zircon_dir, build_dir):
        self._zircon_dir = zircon_dir
        self._command = [gn_binary, '--root=' + zircon_dir, 'refs', build_dir,
                         '--all-toolchains']

    def find_references(self, type, name):
        category_label = '//system/' + type
        base_label = '//system/' + type + '/' + name

        command = self._command + [base_label + ':*']
        try:
            output = subprocess.check_output(command)
        except subprocess.CalledProcessError:
            return None, None

        same_type_references = set()
        other_type_references = set()
        for line in output.splitlines():
            line = line.strip()
            if line.startswith(base_label + ':'):
                continue
            # Remove target name and toolchain.
            line = line[0:line.find(':')]
            if line == category_label:
                continue
            same_type = line.startswith(category_label)
            # Insert 'zircon' directory at the start.
            line = '//zircon' + line[1:]
            refs = same_type_references if same_type else other_type_references
            refs.add(line)

        return same_type_references, other_type_references


    def find_libraries(self, type):
        base = os.path.join(self._zircon_dir, 'system', type)
        def has_zn_build_file(dir):
            build_path = os.path.join(base, dir, 'BUILD.gn')
            if not os.path.isfile(build_path):
                return False
            with open(build_path, 'r') as build_file:
                return '//build/unification/zx_library.gni' not in build_file.read()
        for _, dirs, _ in os.walk(base):
            return filter(has_zn_build_file, dirs)


def main():
    parser = argparse.ArgumentParser('Determines whether libraries can be '
                                     'moved out of the ZN build')
    parser.add_argument('--build-dir',
                        help='Path to the ZN build dir',
                        default=os.path.join(FUCHSIA_ROOT, 'out', 'default.zircon'))
    type = parser.add_mutually_exclusive_group(required=True)
    type.add_argument('--banjo',
                      help='Inspect Banjo libraries',
                      action='store_true')
    type.add_argument('--fidl',
                      help='Inspect FIDL libraries',
                      action='store_true')
    type.add_argument('--ulib',
                      help='Inspect C/C++ libraries',
                      action='store_true')
    type.add_argument('--devlib',
                      help='Inspect C/C++ libraries under dev',
                      action='store_true')
    parser.add_argument('name',
                        help='Name of the library to inspect; if empty, scan '
                             'all libraries of the given type',
                        nargs='?')
    args = parser.parse_args()

    source_dir = FUCHSIA_ROOT
    zircon_dir = os.path.join(source_dir, 'zircon')
    build_dir = os.path.abspath(args.build_dir)

    if sys.platform.startswith('linux'):
        platform = 'linux-x64'
    elif sys.platform.startswith('darwin'):
        platform = 'mac-x64'
    else:
        print('Unsupported platform: %s' % sys.platform)
        return 1
    gn_binary = os.path.join(source_dir, 'prebuilt', 'third_party', 'gn',
                             platform, 'gn')

    finder = Finder(gn_binary, zircon_dir, build_dir)

    if args.fidl:
        type = 'fidl'
    elif args.banjo:
        type = 'banjo'
    elif args.ulib:
        type = 'ulib'
    elif args.devlib:
        type = 'dev/lib'

    # Case 1: a library name is given.
    if args.name:
        name = args.name
        if args.fidl:
            # FIDL library names use the dot separator, but folders use an
            # hyphen: be nice to users and support both forms.
            name = name.replace('.', '-')

        same_type_refs, other_type_refs = finder.find_references(type, name)

        if same_type_refs is None:
            print('Could not find "%s", please check spelling!' % args.name)
            return 1
        elif same_type_refs or other_type_refs:
            print('Nope, there are still references in the ZN build:')
            for ref in sorted(same_type_refs | other_type_refs):
                print('  ' + ref)
            return 2
        else:
            print('Yes you can!')

        return 0

    # Case 2: no library name given.
    print('Warning: this operation can take a while!')
    names = finder.find_libraries(type)
    candidates = {}
    for name in names:
        same_type_refs, other_type_refs = finder.find_references(type, name)
        if other_type_refs:
            continue
        candidates[name] = same_type_refs
    movable = {n for n, r in candidates.iteritems() if not r}
    type_blocked = {n for n, r in candidates.iteritems()
                    if r and set(r).issubset(movable)}
    if type_blocked:
        print('These libraries are only blocked by libraries waiting to be moved:')
        for name in sorted(type_blocked):
            print('  ' + name + ' (' + ','.join(candidates[name]) + ')')
    if movable:
        print('These libraries are free to go:')
        for name in sorted(movable):
            print('  ' + name)
    else:
        print('No library may be moved')

    return 0


if __name__ == '__main__':
    sys.exit(main())
