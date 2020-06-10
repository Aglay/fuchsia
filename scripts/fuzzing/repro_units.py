#!/usr/bin/env python2.7
# Copyright 2019 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import argparse
import sys

from lib.args import ArgParser
from lib.device import Device
from lib.fuzzer import Fuzzer
from lib.host import Host
from lib.factory import Factory


def main():
    factory = Factory()
    parser = ArgParser(
        factory.cli,
        'Runs the named fuzzer on provided test units, or all current test ' +
        'units for the fuzzer. Use \'check-fuzzer\' to see current tests units.'
    )
    args = parser.parse()

    cli = factory.cli
    fuzzer = factory.create_fuzzer(args)
    if fuzzer.repro() == 0:
        cli.error('No matching artifacts found.')


if __name__ == '__main__':
    sys.exit(main())
