#!/usr/bin/env python
# Copyright 2019 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import argparse
import subprocess
import sys

from lib.args import Args
from lib.cipd import Cipd
from lib.device import Device
from lib.fuzzer import Fuzzer
from lib.host import Host


def main():
  parser = Args.make_parser(
      'Lists the fuzzing corpus instances in CIPD for a named fuzzer')
  args = parser.parse_args()

  host = Host()
  device = Device.from_args(host, args)
  fuzzer = Fuzzer.from_args(device, args)
  cipd = Cipd(fuzzer)

  if not cipd.list():
    print 'No corpus instances found in CIPD for ' + str(fuzzer)
    return 1
  else:
    return 0


if __name__ == '__main__':
  sys.exit(main())
