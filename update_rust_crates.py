#!/usr/bin/env python
# Copyright 2017 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import argparse
import os
import paths
import platform
import shutil
import subprocess
import sys
import tempfile


CONFIGS = [
    "lib/fidl/rust/fidl",
    "rust/magenta-rs",
    "rust/magenta-rs/magenta-sys",
]


def get_cargo_bin():
    host_os = platform.system()
    if host_os == "Darwin":
        host_triple = "x86_64-darwin-apple"
    elif host_os == "Linux":
        host_triple = "x86_64-unknown-linux-gnu"
    else:
        raise Exception("Platform not supported: %s" % host_os)
    return os.path.join(paths.FUCHSIA_ROOT, "buildtools", "rust",
                        "rust-%s" % host_triple, "bin", "cargo")


def call_or_exit(args, dir):
    if subprocess.call(args, cwd=dir) != 0:
        raise Exception("Command failed in %s: %s" % (dir, " ".join(args)))


def main():
    parser = argparse.ArgumentParser("Updates third-party Rust crates")
    parser.add_argument("--out",
                        help="Build output directory for host",
                        default="out/debug-x86-64/host_x64")
    parser.add_argument("--cargo-vendor",
                        help="Path to the cargo-vendor command",
                        required=True)
    args = parser.parse_args()

    # Use the root of the tree as the working directory. Ideally a temporary
    # directory would be used, but unfortunately this would break the flow as
    # the configs used to seed the vendor directory must be under a common
    # parent directory.
    base_dir = paths.FUCHSIA_ROOT

    toml_path = os.path.join(base_dir, "Cargo.toml")
    lock_path = os.path.join(base_dir, "Cargo.lock")

    try:
        # Create Cargo.toml.
        def mapper(p): return "\"%s\"" % os.path.join(paths.FUCHSIA_ROOT, p)
        config = '''[workspace]
members = [%s]
''' % ", ".join(map(mapper, CONFIGS))
        with open(toml_path, "w") as config_file:
            config_file.write(config)

        cargo_bin = get_cargo_bin()

        # Generate Cargo.lock.
        lockfile_args = [
            cargo_bin,
            "generate-lockfile",
        ]
        call_or_exit(lockfile_args, base_dir)

        # Populate the vendor directory.
        vendor_args = [
            args.cargo_vendor,
            "-x",
            "--sync",
            lock_path,
            "vendor",
        ]
        call_or_exit(vendor_args, base_dir)
    finally:
        os.remove(toml_path)
        os.remove(lock_path)

    vendor_dir = os.path.join(paths.FUCHSIA_ROOT, "third_party", "rust-crates",
                              "vendor")
    shutil.rmtree(vendor_dir)
    shutil.move(os.path.join(paths.FUCHSIA_ROOT, "vendor"), vendor_dir)


if __name__ == '__main__':
    sys.exit(main())
