#!/usr/bin/env python3
# Copyright 2020 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import argparse
import filecmp
import generate
import os
import shutil
import sys
import tempfile
import unittest
from unittest import mock

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

TMP_DIR_NAME = tempfile.mkdtemp(prefix='tmp_unittest_%s_' % 'GNGenerateTest')


class GNGenerateTest(unittest.TestCase):

    def setUp(self):
        # make sure TMP_DIR_NAME is empty
        if os.path.exists(TMP_DIR_NAME):
            shutil.rmtree(TMP_DIR_NAME)
        os.makedirs(TMP_DIR_NAME)

    def tearDown(self):
        if os.path.exists(TMP_DIR_NAME):
            shutil.rmtree(TMP_DIR_NAME)

    # Use a mock to patch in the command line arguments.
    @mock.patch(
        'argparse.ArgumentParser.parse_args',
        return_value=argparse.Namespace(
            output=TMP_DIR_NAME,
            archive='',
            directory=os.path.join(SCRIPT_DIR, 'testdata'),
            tests=''))
    def testEmptyArchive(self, mock_args):
        # Run the generator.
        generate.main()
        self.verify_contents(TMP_DIR_NAME)

    def verify_contents(self, outdir):
        dcmp = filecmp.dircmp(
            outdir, os.path.join(SCRIPT_DIR, 'golden'), ignore=['bin', 'build'])
        self.verify_contents_recursive(dcmp)
        # check the test_targets template. This is in the build directory,
        # but since it is generated vs. static we add another comparision.
        # fxb/45207 is  tracking fixing the comparison to only look at
        # all generated files.
        generated_file = os.path.join(outdir,'build', 'test_targets.gni')
        golden_file = os.path.join(
            SCRIPT_DIR, 'golden','build','test_targets.gni')
        if not filecmp.cmp(generated_file, golden_file, False):
            self.fail("Generated %s does not match : %s." %
             (generated_file, golden_file))


    def verify_contents_recursive(self, dcmp):
        """Recursively checks for differences between two directories.

        Fails if the directories do not appear to be deeply identical in
        structure and content.

        Args:
            dcmp (filecmp.dircmp): A dircmp of the directories.
        """
        if dcmp.left_only or dcmp.right_only or dcmp.diff_files:
            self.fail("Generated SDK does not match golden files. " \
                "You can run ./update_golden.py to update them.\n" \
                "Only in {}:\n{}\n\n" \
                "Only in {}:\n{}\n\n" \
                "Common different files:\n{}"
                .format(dcmp.left, dcmp.left_only, dcmp.right, dcmp.right_only,
                    dcmp.diff_files))
        for sub_dcmp in dcmp.subdirs.values():
            self.verify_contents_recursive(sub_dcmp)


def TestMain():
    unittest.main()


if __name__ == '__main__':
    TestMain()
