#!/usr/bin/env python2.7
# Copyright 2020 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Validate the sysmgr config for the product being built.

Sysmgr's configuration is provided through the config-data package as a set of
JSON files, which are all individually read by sysmgr at runtime and merged to
form its overall configuration. This tool validates that there are no conflicts
between files, e.g. multiple files providing different component URLs for the
same "services" key. For example, it catches invalid configuration like this::


  file1.config:
  {
    "services": {
      "fuchsia.my.Service": "fuchsia-pkg://fuchsia.com/package_a#meta/foo.cmx"
    }
  }

  file2.config:
  {
    "services": {
      "fuchsia.my.Service": "fuchsia-pkg://fuchsia.com/package_b#meta/bar.cmx"
    }
  }

It also tests for conflicts in the 'diagnostics' key.

The input provided to this tool is expected to be the config-data package
manifest, formatted like this::

  data/some_package/foo=../../src/somewhere/foo
  data/sysmgr/services.config=../../src/sys/sysmgr/config/services.config
  data/sysmgr/file1.config=../../src/bar/file1.config
  data/other_package/baz=../../some/other/path/baz

where the path before the '=' is the destination path in the package, and the
path after the '=' is the source file (rebased to the root build directory).
"""

import argparse
import json
import os
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        '--stamp', metavar='FILE', help='Touch FILE at the end.', required=True)
    parser.add_argument(
        '--depfile',
        metavar='FILE',
        help='Write a GN depfile to the given path',
        required=True)
    parser.add_argument(
        '--merged',
        metavar='FILE',
        help='Write a snapshot of this sysmgr config to the given path',
        required=True)
    parser.add_argument('manifest', help='config-data package manifest')
    args = parser.parse_args()

    with open(args.manifest, 'r') as f:
        lines = f.read().splitlines()

    # Build a list of all the source paths that contribute to sysmgr's config-data
    sysmgr_lines = [line for line in lines if line.startswith('data/sysmgr/')]
    sysmgr_config_files = [line.split('=')[1] for line in sysmgr_lines]

    # Parse all config files
    configs = []
    for config_file in sysmgr_config_files:
        with open(config_file, 'r') as f:
            configs.append(json.load(f))

    # Build a list of all conflicts, rather than
    # failing immediately on the first conflict. This allows us to print a more
    # useful build failure so that developers don't need to play whack-a-mole with
    # multiple conflicts.
    seen_services = {}
    service_conflicts = {}
    seen_diag = None
    diag_conflicts = []
    for config in configs:
        diag = config.get('diagnostics')
        if diag is not None:
            if seen_diag is None:
                seen_diag = config_file
            else:
                if not diag_conflicts:
                    diag_conflicts.append(seen_diag)
                diag_conflicts.append(config_file)

        services = config.get('services')
        if not services:
            continue
        for service in services.iterkeys():
            if service in seen_services:
                if not service in service_conflicts:
                    service_conflicts[service] = [seen_services[service]]
                service_conflicts[service].append(config_file)
            else:
                seen_services[service] = config_file

    # If any conflicts were detected, print a useful error message and then
    # exit.
    if service_conflicts or diag_conflicts:
        print('Error: conflicts detected in sysmgr configuration')
        for service, config_files in service_conflicts.items():
            print(
                'Duplicate configuration for service {} in files: {}'.format(
                    service, ', '.join(config_files)))
        if diag_conflicts:
            print(
                'Duplicate diagnostics config in files: {}'.format(
                    ', '.join(diag_conflicts)))
        return 1

    # Create a single merged configuration analogous to sysmgr's init itself.
    merged_config = {}
    for config in configs:
        for category, values in config.items():
            existing = merged_config.get(category)
            if type(existing) is dict:
                merged_config[category].update(values)
            elif type(existing) is list:
                merged_config[category] += values
            else:
                merged_config[category] = values

    with open(args.merged, 'w') as f:
        json.dump(
            merged_config, f, indent=2, separators=(',', ': '), sort_keys=True)

    # Write the depfile, which is a Makefile format file that has a single output
    # (the stamp file) and lists all input files as dependencies.
    with open(args.depfile, 'w') as f:
        f.write('{}: {}\n'.format(args.stamp, ' '.join(sysmgr_config_files)))

    # Write the stampfile.
    with open(args.stamp, 'w') as f:
        os.utime(f.name, None)

    return 0


if __name__ == '__main__':
    sys.exit(main())
