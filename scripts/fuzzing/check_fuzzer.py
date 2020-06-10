#!/usr/bin/env python2.7
# Copyright 2019 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import sys

import lib.command as command
from lib.args import ArgParser
from lib.factory import Factory


def main():
    factory = Factory()
    parser = ArgParser(
        factory.cli, 'Reports status for the fuzzer matching NAME if ' +
        'provided, or for all running fuzzers.  Status includes execution ' +
        'state, corpus size, and number of artifacts.')
    parser.require_name(False)
    command.check_fuzzer(parser.parse(), factory=factory)


if __name__ == '__main__':
    sys.exit(main())
