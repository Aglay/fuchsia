#!/usr/bin/env python
# Copyright 2018 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import argparse
import os
import sys
import json

def main():
    parser = argparse.ArgumentParser(description=("List all targets in the pushable available set"))
    parser.add_argument('--build-dir', action='store', required=True)

    args = parser.parse_args()
    with open(os.path.join(args.build_dir, "packages.json")) as f:
      data = json.load(f)

    available_build_packages = set(data["available"])

    with open(os.path.join(args.build_dir, "amber-files", "repository", "targets.json")) as f:
      data = json.load(f)

    published_packages = set([s.split('/')[1] for s in data['signed']['targets'].keys()])

    available_packages = published_packages & available_build_packages

    for tgt in available_packages:
      print(tgt)

if __name__ == '__main__':
    sys.exit(main())
