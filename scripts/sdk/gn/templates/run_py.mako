#!/usr/bin/env -S  python2.7 -B
# Copyright 2020 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""
Runs the GN SDK tests
usage: run.py [-h] [--proj_dir PROJ_DIR] [--out_dir OUT_DIR]

optional arguments:
  -h, --help           show this help message and exit
  --proj_dir PROJ_DIR  Path to the test project directory
  --out_dir OUT_DIR    Path to the out directory

PROJ_DIR defaults to the same directory run.py is contained in
OUT_DIR defaults to ./out/default relative to the run.py
"""

import argparse
import imp
import os
import platform
from subprocess import Popen, PIPE
import sys
import unittest

ARCHES = [
    'arm64',
    'x64',
]

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_OUT_DIR = os.path.join(SCRIPT_DIR, 'out')

FUCHSIA_ROOT = '${data.fuchsia_root}'

# Import test_generate
TEST_GEN_PATH = os.path.join(
    FUCHSIA_ROOT, 'scripts', 'sdk', 'gn', 'test_generate.py')
test_generate = imp.load_source('test_generate', TEST_GEN_PATH)

# bash_tests constants
BASH_TESTS_PATH = os.path.join(
    FUCHSIA_ROOT, 'scripts', 'sdk', 'gn', 'bash_tests', 'run_bash_tests.sh')

# gn and ninja constants
DEFAULT_HOST = 'linux-x64'
THIRD_PARTY_DIR = os.path.join(FUCHSIA_ROOT, 'prebuilt', 'third_party')
DEFAULT_GN_BIN = os.path.join(THIRD_PARTY_DIR, 'gn', DEFAULT_HOST, 'gn')
DEFAULT_NINJA_BIN = os.path.join(
    THIRD_PARTY_DIR, 'ninja', DEFAULT_HOST, 'ninja')
DEFAULT_CLANG_DIR = os.path.join(THIRD_PARTY_DIR, 'clang', DEFAULT_HOST)

# host test constants
HOST_TEST_PATH = os.path.join(SCRIPT_DIR, 'tests', 'run-host-tests.sh')


class GnTester(object):
    """Class for GN SDK test setup, execution, and cleanup."""

    def __init__(self, gn, ninja, clang, proj_dir, out_dir):
        self._test_failed = False
        # Paths for building sample project
        self.gn = gn
        self.ninja = ninja
        self.clang = clang
        self.proj_dir = proj_dir
        self.out_dir = out_dir
        # Import gen_fidl_response_file_unittest
        GEN_FIDL_RESP_FILE_TEST_PATH = os.path.join(
            proj_dir, 'tests', 'gen_fidl_response_file_unittest.py')
        self.gen_fidl_response_file_unittest = imp.load_source(
            'gen_fidl_response_file_unittest', GEN_FIDL_RESP_FILE_TEST_PATH)

    def _run_unit_test(self, test_module):
        loader = unittest.TestLoader()
        tests = loader.loadTestsFromModule(test_module)
        suite = unittest.TestSuite()
        suite.addTests(tests)
        runner = unittest.TextTestRunner()
        result = runner.run(suite)
        if result.failures or result.errors:
            raise AssertionError('Unit test failed.')

    def _generate_test(self):
        """Test generate.py via python test class."""
        self._run_unit_test(test_generate)
        print "Generate tests passed."

    def _gen_fild_resp_file_unittest(self):
        self._run_unit_test(self.gen_fidl_response_file_unittest)
        print "FIDL response file unit test passed."

    def _bash_tests(self):
        self._run_cmd([BASH_TESTS_PATH])
        print "Bash tests passed."

    def _run_cmd(self, args, cwd=None):
        job = Popen(args, cwd=cwd, stdout=PIPE)
        (stdoutdata, stderrdata) = job.communicate()
        print stdoutdata
        if job.returncode:
            print stderrdata
            msg = 'Command returned non-zero exit code: %s' % job.returncode
            raise AssertionError(msg)
        return (stdoutdata, stderrdata)

    def _run_test(self, test, *args):
        try:
            getattr(self, test)(*args)
        except Exception as err:
            print "ERROR Running test %s: %s" % (test, err)
            self._test_failed = True

    def _build_test_project(self, arch):
        """Builds the test project for given architecture."""
        self._invoke_gn(arch)
        self._invoke_ninja(arch)
        print "Test project for %s built successfully" % arch

    def _invoke_gn(self, arch):
        """Invokes GN targeting the given architecture.

        Example invocation:
          "gn" gen out --args='target_os="fuchsia" target_cpu="arm64"'
        """
        # Invoke the gn binary and "gen" command e.g. `gn gen`
        invocation = [
            self.gn, "gen",
            os.path.join(self.out_dir, arch),
            "--args=target_cpu=\"{cpu}\" target_os=\"fuchsia\" clang_base_path=\"{clang}\""
            .format(cpu=arch, clang=self.clang)
        ]
        # invoke command
        print 'Running gn gen: "%s"' % ' '.join(invocation)
        self._run_cmd(invocation, cwd=self.proj_dir)

    def _invoke_ninja(self, arch):
        """Invokes Ninja to build default and tests targets.

        Args:
           arch: The target architecture to build.
        """
        invocation = [
            # Invoke the ninja binary
            self.ninja,
            # Add Ninja flag to command e.g. `-C`
            "-C",
            # Add output directory to command
            os.path.join(self.out_dir, arch),
            # Build the default and test targets
            "default",
            "tests"
        ]
        print 'Running ninja: "%s"' % ' '.join(invocation)
        return self._run_cmd(invocation, cwd=self.proj_dir)

    def _verify_package_depfile(self, arch):
        print('Running package dep file verification test')
        # Build test project
        self._build_test_project(arch)
        # Verify package dep file for built project
        package_dep_file_contents = "gen/tests/package/package/package.archive_manifest: lib/libfdio.so lib/libunwind.so.1 lib/libc++abi.so.1 hello_bin lib/ld.so.1 lib/libc++.so.2"
        dep_filepath = os.path.join(
            self.out_dir, arch, "gen", "tests", "package", "package_stamp.d")
        with open(dep_filepath, 'r') as dep_file:
            dep_file_contents = dep_file.read()
            if dep_file_contents != package_dep_file_contents:
                msg = 'Dep file %s' % dep_filepath
                msg += 'expected to have contents:\n\n%s\n\n' % package_dep_file_contents
                msg += 'but got \n\n%s\n\n' % dep_file_contents
                raise AssertionError(msg)

    def _verify_rebuild_noop(self, arch):
        """Builds each architecture twice, confirming the second is a noop."""
        # Build test project initially
        self._build_test_project(arch)
        # Build test project a second time
        self._invoke_gn(arch)
        (stdout, stderr) = self._invoke_ninja(arch)
        # Verify that the second test project build is a noop for ninja
        ninja_no_work_string = 'ninja: no work to do.'
        if not ninja_no_work_string in stdout:
            msg = 'Rebuild of test project did not result in noop.\n'
            msg += 'Expected std out to contain "%s" but got:\n\n' % ninja_no_work_string
            msg += '"%s"' % stdout
            raise AssertionError(msg)
        print "Test project rebuilt successfully"

    def _run_build_tests(self):
        """Run the build tests, once per architecture."""
        for arch in ARCHES:
            self._run_test("_build_test_project", arch)
            self._run_test("_verify_package_depfile", arch)
            self._run_test("_verify_rebuild_noop", arch)
            self._run_test("_host_tests", arch)

    def _host_tests(self, arch):
        # Build test project to generate far archives
        self._build_test_project(arch)
        invocation = [
            HOST_TEST_PATH,
            os.path.join("out", arch, 'all_host_tests.txt')
        ]
        print 'Running run-host-tests.sh: "%s"' % ' '.join(invocation)
        self._run_cmd(invocation, cwd=self.proj_dir)

    def run(self):
        self._run_test("_generate_test")
        self._run_test("_gen_fild_resp_file_unittest")
        self._run_test("_bash_tests")
        if platform.system() == 'Darwin':
            print "The GN SDK does not support building on macOS."
        else:
            self._run_build_tests()
        return self._test_failed


def main():
    parser = argparse.ArgumentParser(description='Runs the GN SDK tests')
    parser.add_argument(
        '--gn',
        help='Path to the GN tool; defaults to %s' % DEFAULT_GN_BIN,
        default=DEFAULT_GN_BIN)
    parser.add_argument(
        '--ninja',
        help='Path to the Ninja tool; otherwise found in PATH',
        default=DEFAULT_NINJA_BIN)
    parser.add_argument(
        '--clang',
        help='Path to Clang base path, defaults to %s' % DEFAULT_CLANG_DIR,
        default=os.path.join(SCRIPT_DIR, DEFAULT_CLANG_DIR))
    parser.add_argument(
        '--proj-dir',
        help='Path to the test project directory, defaults to %s' % SCRIPT_DIR,
        default=SCRIPT_DIR)
    parser.add_argument(
        '--out-dir',
        help='Path to the out directory, defaults to %s' % DEFAULT_OUT_DIR,
        default=DEFAULT_OUT_DIR)
    args = parser.parse_args()
    if GnTester(
            gn=args.gn,
            ninja=args.ninja,
            clang=args.clang,
            proj_dir=args.proj_dir,
            out_dir=args.out_dir,
    ).run():
        print "ERROR: One or more tests failed."
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
