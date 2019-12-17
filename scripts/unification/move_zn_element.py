#!/usr/bin/env python2.7
# Copyright 2019 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import argparse
import fileinput
import json
import os
import re
import subprocess
import sys


SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
SCRIPTS_DIR = os.path.dirname(SCRIPT_DIR)
FUCHSIA_ROOT = os.path.dirname(SCRIPTS_DIR)
FX = os.path.join(SCRIPTS_DIR, 'fx')

SCRIPT_LABEL = '//' + os.path.relpath(os.path.abspath(__file__),
                                      start=FUCHSIA_ROOT)


class Type(object):
    DRIVER = 'zx_driver'
    EXECUTABLE = 'zx_executable'
    TEST = 'zx_test'
    TEST_DRIVER = 'zx_test_driver'


BINARY_TYPES = {
    Type.DRIVER: 'driver_module',
    Type.EXECUTABLE: 'executable',
    Type.TEST: 'test',
    Type.TEST_DRIVER: 'driver_module',
}


def run_command(command):
    return subprocess.check_output(command, cwd=FUCHSIA_ROOT)


def locate_build_files(base):
    result = []
    for root, dirs, files in os.walk(os.path.join(FUCHSIA_ROOT, 'zircon',
                                    'system', base)):
        for file in files:
            if file == 'BUILD.gn':
                result.append(os.path.join(root, file))
    return result


def transform_build_file(build):
    # First pass: identify contents of the build file.
    binaries = []
    has_test_binaries = False
    has_drivers = False
    binary_types = BINARY_TYPES.keys()
    unclear_types = ['library']
    n_lines = 0
    with open(build, 'r') as build_file:
        lines = build_file.readlines()
        n_lines = len(lines)
        for line in lines:
            match = re.match(r'\A([^\(]+)\("([^"]+)"\)', line)
            if match:
                type, name = match.groups()
                if type in binary_types:
                    binaries.append(name)
                    if type == Type.TEST:
                        has_test_binaries = True
                    if type == Type.DRIVER or type == Type.TEST_DRIVER:
                        has_drivers = True
                if type in unclear_types:
                    print('Warning: target ' + name + ' of type ' + type + ' '
                          'needs to be manually converted.')

    # Second pass: rewrite contents to match GN build standards.
    imports_added = False
    for line in fileinput.FileInput(build, inplace=True):
        # Apply required edits.
        # Update target types.
        starting_type = ''
        for type in binary_types:
            new_type_line = line.replace(type, BINARY_TYPES[type])
            if new_type_line != line:
                starting_type = type
                line = new_type_line
                break
        # Remove references to libzircon.
        if '$zx/system/ulib/zircon' in line and not 'zircon-internal' in line:
            line = ''
        # Update references to libraries.
        line = line.replace('$zx/system/ulib', '//zircon/public/lib')
        line = line.replace('$zx/system/dev/lib', '//zircon/public/lib')
        # Update references to Zircon in general.
        line = line.replace('$zx', '//zircon')
        # Print the line, if any content is left.
        if line:
            sys.stdout.write(line)

        # Insert required imports at the start of the file.
        if not line.strip() and not imports_added:
            imports_added = True
            sys.stdout.write('##########################################\n')
            sys.stdout.write('# Though under //zircon, this build file #\n')
            sys.stdout.write('# is meant to be used in the Fuchsia GN  #\n')
            sys.stdout.write('# build.                                 #\n')
            sys.stdout.write('# See fxb/36139.                         #\n')
            sys.stdout.write('##########################################\n')
            sys.stdout.write('\n')
            sys.stdout.write('assert(!defined(zx) || zx != "/", "This file can only be used in the Fuchsia GN build.")\n')
            sys.stdout.write('\n')
            if has_drivers:
                sys.stdout.write('import("//build/config/fuchsia/rules.gni")\n')
            if has_test_binaries:
                sys.stdout.write('import("//build/test.gni")\n')
            sys.stdout.write('import("//build/unification/images/migrated_manifest.gni")\n')
            sys.stdout.write('\n')

        # Add extra parameters to tests.
        if starting_type == Type.TEST:
            sys.stdout.write('  # Dependent manifests unfortunately cannot be marked as `testonly`.\n')
            sys.stdout.write('  # Remove when converting this file to proper GN build idioms.\n')
            sys.stdout.write('  testonly = false\n')

        if starting_type == Type.TEST_DRIVER:
            sys.stdout.write('  test = true\n')

        if starting_type in [Type.DRIVER, Type.TEST_DRIVER]:
            sys.stdout.write('  defines = [ "_ALL_SOURCE" ]\n')
            sys.stdout.write('  configs += [ "//build/config/fuchsia:enable_zircon_asserts" ]\n')
            sys.stdout.write('  configs -= [ "//build/config/fuchsia:no_cpp_standard_library" ]\n')
            sys.stdout.write('  configs += [ "//build/config/fuchsia:static_cpp_standard_library" ]\n')

        if starting_type in [Type.EXECUTABLE, Type.TEST]:
            sys.stdout.write('  configs += [ "//build/unification/config:zircon-migrated" ]\n')


    # Third pass: add manifest targets at the end of the file.
    with open(build, 'a') as build_file:
        for binary in binaries:
            build_file.write('\n')
            build_file.write('migrated_manifest("' + binary + '-manifest") {\n')
            build_file.write('  deps = [\n')
            build_file.write('    ":' + binary + '",\n')
            build_file.write('  ]\n')
            build_file.write('}\n')

    # Format the file.
    run_command([FX, 'format-code', '--files=' + build])

    return 0


def main():
    parser = argparse.ArgumentParser(
            description='Moves a binary from ZN to GN.')
    commands = parser.add_subparsers()

    convert_parser = commands.add_parser('convert',
                                         help='Migrate from ZN to GN')
    convert_parser.add_argument('binary',
                        help='The binary under //zircon/system to migrate, '
                             'e.g. uapp/audio, utest/fit, dev/bus/pci')
    convert_parser.set_defaults(func=run_convert)

    list_parser = commands.add_parser('list',
                                      help='List available binaries')
    list_parser.add_argument('--build-dir',
                             help='path to the ZN build dir',
                             default=os.path.join(FUCHSIA_ROOT, 'out', 'default.zircon'))
    list_parser.set_defaults(func=run_list)

    args = parser.parse_args()
    args.func(args)


def run_convert(args):
    # Check that the fuchsia.git tree is clean.
    diff = run_command(['git', 'status', '--porcelain'])
    if diff:
        print('Please make sure your tree is clean before running this script')
        print(diff)
        return 1

    # Identify the affected build files.
    build_files = locate_build_files(args.binary)
    if not build_files:
        print('Error: could not find any files for ' + args.binary)
        return 1

    # Confirm with the user that these are the files they want to convert.
    print('The following build file(s) will be converted:')
    for file in build_files:
        print(' - ' + os.path.relpath(file, FUCHSIA_ROOT))
    go_ahead = raw_input('Proceed? (Y/n) ').lower().strip()
    if go_ahead != 'y' and go_ahead != '':
        print('User disagrees, exiting')
        return 0

    # Convert the build files.
    for file in build_files:
        transform_build_file(file)

    # Create a commit.
    id = args.binary.replace('/', '_')
    run_command(['git', 'checkout', '-b', 'gn-move-' + id, 'JIRI_HEAD'])
    run_command(['git', 'add', '.'])
    message = [
        '[unification] Move //zircon/system/' + args.binary + ' to the GN build',
        '',
        'Generated with: ' + SCRIPT_LABEL,
        '',
        'Bug: 36139'
    ]
    commit_command = ['git', 'commit', '-a']
    for line in message:
        commit_command += ['-m', line]
    run_command(commit_command)

    print('Base change is ready. Please attempt to build a full system to '
          'identify further required changes.')

    return 0


def run_list(args):
    targets = set()
    for arch in ['arm64', 'x64']:
        manifest_path = os.path.join(args.build_dir,
                                     'legacy_unification-%s.json' % arch)
        with open(manifest_path, 'r') as manifest_file:
            data = json.load(manifest_file)
        for item in data:
            if item['name'].startswith('lib.'):
                # Libraries will be migrated through a different process.
                continue
            label = item['label']
            # Labels are always full, i.e. "//foo/bar:blah(//toolchain)".
            label = label[0:label.index('(')]
            label = label[0:label.index(':')]
            if not label.startswith('//system'):
                continue
            label = label[len('//system/'):]
            targets.add(label)
    for target in sorted(targets):
        print(target)


if __name__ == '__main__':
    sys.exit(main())
