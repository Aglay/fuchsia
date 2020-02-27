#!/usr/bin/env python2.7
# Copyright 2020 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import argparse
import fileinput
import json
import os
import re
import sys
import tempfile


from common import (FUCHSIA_ROOT, run_command, is_tree_clean, fx_format,
                    is_in_fuchsia_project)
from get_library_stats import get_library_stats, Sdk

SCRIPT_LABEL = '//' + os.path.relpath(os.path.abspath(__file__),
                                      start=FUCHSIA_ROOT)


def main():
    parser = argparse.ArgumentParser(
            description='Moves a C/C++ library from //zircon to //sdk')
    parser.add_argument('--lib',
                        help='Name of the library folder to migrate, e.g. '
                             'ulib/foo or dev/lib/bar',
                        action='append')
    args = parser.parse_args()

    if not args.lib:
        print('Need to specify at least one library.')
        return 1

    # Check that the fuchsia.git tree is clean.
    if not is_tree_clean():
        return 1

    # Whether any of the libraries required changes in a non-fuchsia.git project.
    global_cross_project = False

    movable_libs = {}
    for lib in args.lib:
        # Verify that the library may be migrated at this time.
        build_path = os.path.join(FUCHSIA_ROOT, 'zircon', 'system', lib,
                                  'BUILD.gn')
        base_label = '//zircon/system/' + lib
        stats = get_library_stats(build_path)

        # No kernel!
        has_kernel = len([s for s in stats if s.kernel]) != 0
        if has_kernel:
            print('Some libraries are used in the kernel and may not be '
                  'migrated at the moment, ignoring ' + lib)
            continue

        # Only source libraries!
        non_source_sdk = len([s for s in stats if s.sdk != Sdk.SOURCE]) != 0
        if non_source_sdk:
            print('Can only convert libraries exported as "sources" for now, '
                  'ignoring ' + lib)
            continue

        # Gather build files with references to the library.
        build_files = []
        cross_project = False
        for base, _, files in os.walk(FUCHSIA_ROOT):
            for file in files:
                if file != 'BUILD.gn':
                    continue
                file_path = os.path.join(base, file)
                with open(file_path, 'r') as build_file:
                    content = build_file.read()
                    for name in [s.name for s in stats]:
                        reference = '"//zircon/public/lib/' + name + '"'
                        if reference in content:
                            build_files.append(file_path)
                            if not is_in_fuchsia_project(file_path):
                                cross_project = True
                            break

        movable_libs[lib] = (build_path, base_label, stats, build_files, cross_project)

    if not movable_libs:
        print('Could not find any library to convert, aborting')
        return 1

    solo_libs = [n for n, t in movable_libs.items() if t[4]]
    if solo_libs and len(movable_libs) > 1:
        print('These libraries may only be moved in a dedicated change: ' +
              ', '.join(solo_libs))
        return 1

    for lib, (build_path, base_label, stats, build_files, cross_project) in movable_libs.items():
        # Rewrite the library's build file.
        import_added = False
        for line in fileinput.FileInput(build_path, inplace=True):
            # Remove references to libzircon.
            if '$zx/system/ulib/zircon' in line and not 'zircon-internal' in line:
                line = ''
            # Update references to libraries.
            line = line.replace('$zx/system/ulib', '//zircon/public/lib')
            line = line.replace('$zx/system/dev/lib', '//zircon/public/lib')
            # Update known configs.
            line = line.replace('$zx_build/public/gn/config:static-libc++',
                                '//build/config/fuchsia:static_cpp_standard_library')
            # Update references to Zircon in general.
            line = line.replace('$zx', '//zircon')
            # Update deps on libdriver.
            line = line.replace('//zircon/public/lib/driver',
                                '//src/devices/lib/driver')
            # Remove header target specifier.
            line = line.replace('.headers', '')
            line = line.replace(':headers', '')
            sys.stdout.write(line)

            if not line.strip() and not import_added:
                import_added = True
                sys.stdout.write('##########################################\n')
                sys.stdout.write('# Though under //zircon, this build file #\n')
                sys.stdout.write('# is meant to be used in the Fuchsia GN  #\n')
                sys.stdout.write('# build.                                 #\n')
                sys.stdout.write('# See fxb/36548.                         #\n')
                sys.stdout.write('##########################################\n')
                sys.stdout.write('\n')
                sys.stdout.write('assert(!defined(zx) || zx != "/", "This file can only be used in the Fuchsia GN build.")\n')
                sys.stdout.write('\n')
                sys.stdout.write('import("//build/unification/zx_library.gni")\n')
                sys.stdout.write('\n')
        fx_format(build_path)

        # Edit references to the library.
        for file_path in build_files:
            for line in fileinput.FileInput(file_path, inplace=True):
                for name in [s.name for s in stats]:
                    new_label = '"' + base_label
                    if os.path.basename(new_label) != name:
                        new_label = new_label + ':' + name
                    new_label = new_label + '"'
                    line = re.sub('"//zircon/public/lib/' + name + '"',
                                  new_label, line)
                sys.stdout.write(line)
            fx_format(file_path)

        # Generate an alias for the library under //zircon/public/lib if a soft
        # transition is necessary.
        if cross_project:
            global_cross_project = True

            alias_path = os.path.join(FUCHSIA_ROOT, 'build', 'unification',
                                      'zircon_library_mappings.json')
            with open(alias_path, 'r') as alias_file:
                data = json.load(alias_file)
            for s in stats:
                data.append({
                    'name': s.name,
                    'sdk': s.sdk_publishable,
                    'label': base_label + ":" + s.name,
                })
            data = sorted(data, key=lambda item: item['name'])
            with open(alias_path, 'w') as alias_file:
                json.dump(data, alias_file, indent=2, sort_keys=True,
                          separators=(',', ': '))

        # Remove the reference in the ZN aggregation target.
        aggregation_path = os.path.join(FUCHSIA_ROOT, 'zircon', 'system',
                                        os.path.dirname(lib), 'BUILD.gn')
        folder = os.path.basename(lib)
        for line in fileinput.FileInput(aggregation_path, inplace=True):
            for name in [s.name for s in stats]:
                if (not '"' + folder + ':' + name + '"' in line and
                    not '"' + folder + '"' in line):
                    sys.stdout.write(line)

    # Create a commit.
    libs = sorted(movable_libs.keys())
    under_libs = [l.replace('/', '_') for l in libs]
    branch_name = 'lib-move-' + under_libs[0]
    lib_name = '//zircon/system/' + libs[0]
    if len(libs) > 1:
        branch_name += '-and-co'
        lib_name += ' and others'
    run_command(['git', 'checkout', '-b', branch_name, 'JIRI_HEAD'])
    run_command(['git', 'add', FUCHSIA_ROOT])
    message = [
        '[unification] Move ' + lib_name + ' to the GN build',
        '',
    ] + ['//zircon/system/' + l for l in libs] + [
        '',
        'Generated with ' + SCRIPT_LABEL,
        '',
        'Bug: 36548'
    ]
    fd, message_path = tempfile.mkstemp()
    with open(message_path, 'w') as message_file:
        message_file.write('\n'.join(message))
    commit_command = ['git', 'commit', '-a', '-F', message_path]
    run_command(commit_command)
    os.close(fd)
    os.remove(message_path)

    if global_cross_project:
        print('*** Warning: multiple Git projects were affected by this move!')
        print('Run jiri status to view affected projects.')
        print('Staging procedure:')
        print(' - use "jiri upload" to start the review process on the fuchsia.git change;')
        print(' - prepare dependent CLs for each affected project and get them review;')
        print(' - when the fuchsia.git change has rolled into GI, get the other CLs submitted;')
        print(' - when these CLs have rolled into GI, prepare a change to remove the forwarding')
        print('   targets under //build/unification/zircon_library_mappings.json')
    else:
        print('Change is ready, use "jiri upload" to start the review process.')

    return 0


if __name__ == '__main__':
    sys.exit(main())
