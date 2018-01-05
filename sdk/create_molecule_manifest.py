#!/usr/bin/env python
# Copyright 2017 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import argparse
import json
import os
import sys

from create_atom_manifest import gather_dependencies


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out',
                        help='Path to the output file',
                        required=True)
    parser.add_argument('--deps',
                        help='List of manifest paths for the included elements',
                        nargs='*')
    args = parser.parse_args()

    (_, atoms) = gather_dependencies(args.deps)
    manifest = {
        'type': 'molecule',
        'atoms': map(lambda a: a.json, list(atoms)),
    }
    with open(os.path.abspath(args.out), 'w') as out:
        json.dump(manifest, out, indent=2, sort_keys=True)


if __name__ == '__main__':
    sys.exit(main())
