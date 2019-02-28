#!/usr/bin/env python
# Copyright 2018 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import argparse
import os
from subprocess import check_output, Popen
import sys


SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))


ARCHES = [
% for arch in data.arches:
    '${arch.short_name}',
% endfor
]


def program_exists(name):
    """Returns True if an executable with the name exists"""
    if len(name) > 0 and name[0] == '/':
        return os.path.isfile(name) and os.access(name, os.X_OK)
    for path in os.environ["PATH"].split(os.pathsep):
        fname = os.path.join(path, name)
        if os.path.isfile(fname) and os.access(fname, os.X_OK):
            return True
    return False


class BazelTester(object):

    def __init__(self, without_sdk, with_ignored, bazel_bin,
                 optional_flags=[]):
        self.without_sdk = without_sdk
        self.with_ignored = with_ignored
        self.bazel_bin = bazel_bin
        self.optional_flags = optional_flags


    def _add_bazel_flags(self, command):
        # The following flag is needed because some Dart build rules use a
        # `cfg = "data"` construct that's now an error.
        # TODO: remove this flag when we don't build Dart stuff in this SDK.
        command += ['--incompatible_disallow_data_transition=false']


    def _invoke_bazel(self, command, targets):
        command = [self.bazel_bin, command, '--keep_going']
        self._add_bazel_flags(command)
        command += self.optional_flags
        command += targets
        job = Popen(command, cwd=SCRIPT_DIR)
        job.communicate()
        return job.returncode


    def _build(self, targets):
        return self._invoke_bazel('build', targets)


    def _test(self, targets):
        return self._invoke_bazel('test', targets)


    def _query(self, query):
        command = [self.bazel_bin, 'query', query]
        self._add_bazel_flags(command)
        return set(check_output(command, cwd=SCRIPT_DIR).splitlines())


    def run(self):
        if not self.without_sdk:
            # Build the SDK contents.
            print('Building SDK contents')
            if self._build(['@fuchsia_sdk//...']):
                return False

        targets = ['//...']
        if not self.with_ignored:
            # Identify and remove ignored targets.
            all_targets = self._query('//...')
            ignored_targets = self._query('attr("tags", "ignored", //...)')
            if ignored_targets:
                # Targets which depend on an ignored target should be ignored too.
                all_ignored_targets = set()
                for target in ignored_targets:
                    all_ignored_targets.add(target)
                    dep_query = 'rdeps("//...", "{}")'.format(target)
                    dependent_targets = self._query(dep_query)
                    all_ignored_targets.update(dependent_targets)
                print('Ignored targets:')
                for target in sorted(all_ignored_targets):
                    print(' - ' + target)
                targets = list(all_targets - all_ignored_targets)

        # Build the tests targets.
        print('Building test targets')
        if self._build(targets):
            return False

        # Run tests.
        args = ('attr("tags", "^((?!compile-only).)*$",' +
                ' kind(".*test rule", //...))')
        test_targets = list(self._query(args))
        print('Running test targets')
        return self._test(test_targets) == 0


def main():
    parser = argparse.ArgumentParser(
        description='Runs the SDK tests')
    parser.add_argument('--no-sdk',
                        help='If set, SDK targets are not built.',
                        action='store_true')
    parser.add_argument('--ignored',
                        help='If set, ignored tests are run too.',
                        action='store_true')
    parser.add_argument('--bazel',
                        help='Path to the Bazel tool',
                        default='bazel')
    parser.add_argument('--once',
                        help='Whether to only run tests once',
                        action='store_true')
    args = parser.parse_args()

    if not program_exists(args.bazel):
        print('"%s": command not found' % (args.bazel))
        return 1

    def print_test_start(arch, cpp_version):
        print('')
        print('-----------------------------------')
        print('| Testing %s / %s' % (arch, cpp_version))
        print('-----------------------------------')

    for arch in ARCHES:
        print_test_start(arch, 'C++14')
        config_flags = ['--config=fuchsia_%s' % arch]
        cpp14_flags = ['--cxxopt=-Wc++14-compat', '--cxxopt=-Wc++17-extensions']
        if not BazelTester(args.no_sdk, args.ignored, args.bazel,
                           optional_flags=config_flags + cpp14_flags).run():
            return 1

        if args.once:
            print('Single iteration requested, done.')
            break

        print_test_start(arch, 'C++17')
        cpp17_flags = ['--cxxopt=-std=c++17', '--cxxopt=-Wc++17-compat']
        if not BazelTester(args.no_sdk, args.ignored, args.bazel,
                           optional_flags=config_flags + cpp17_flags).run():
            return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
