#!/usr/bin/env python
# Copyright 2018 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import argparse
import json
import os.path
import paths
import sys
from prepreprocess_build_packages import PackageImportsResolver, PackageLabelObserver

def parse_product(product, build_packages):
    """
    product - a path to a JSON product file to parse
    build_packages - a dict that collects merged sets
    """
    product = os.path.join(paths.FUCHSIA_ROOT, product)
    build_packages["files_read"].add(product)

    with open(product) as f:
        for k, v in json.load(f).items():
            if k == "monolith":
                build_packages[k].update(v)
                continue
            if k == "preinstall":
                build_packages[k].update(v)
                continue
            if k == "available":
                build_packages[k].update(v)
                continue
            sys.stderr.write("Invalid product key in %s: %s\n" % (product, k))


def preprocess_packages(packages):
    observer = PackageLabelObserver()
    imports_resolver = PackageImportsResolver(observer)
    imported = imports_resolver.resolve_imports(packages)

    if imports_resolver.errored():
        raise ImportError

    if imported == None:
        return None

    return observer.json_result


def main():
    parser = argparse.ArgumentParser(description="""
Merge a list of product definitions to unique lists of GN labels:

monolith   - the list of packages included in the base system images
preinstall - the list of packages preinstall, but not part of OTA
available  - the list of packages installable and updatable
host_tests - host tests collected from all above package sets
data_deps  - additional labels to build, such as host tools
files_read - a list of files used to compute all of the above
""")
    parser.add_argument("--monolith",
                        help="List of package definitions for the monolith",
                        required=True)
    parser.add_argument("--preinstall",
                        help="List of package definitions for preinstalled packages",
                        required=True)
    parser.add_argument("--available",
                        help="List of package definitions for available packages",
                        required=True)
    parser.add_argument("--legacy-products",
                        help="List of legacy product definitions",
                        required=False)
    args = parser.parse_args()

    build_packages = {
        "monolith": set(),
        "preinstall": set(),
        "available": set(),
        "files_read": set(),
    }


    # Parse monolith, preinstall, and available sets.
    build_packages["monolith"].update(json.loads(args.monolith))
    build_packages["preinstall"].update(json.loads(args.preinstall))
    build_packages["available"].update(json.loads(args.available))

    # Merge in the legacy product configurations, if set
    [parse_product(product, build_packages) for product in
            json.loads(args.legacy_products)]

    try:
        monolith_results = preprocess_packages(list(build_packages["monolith"]))
        preinstall_results = preprocess_packages(list(build_packages["preinstall"]))
        available_results = preprocess_packages(list(build_packages["available"]))
    except ImportError:
        return 1

    host_tests = set()
    data_deps = set()
    for res in (monolith_results, preinstall_results, available_results):
        if res is None:
            continue
        if res["host_tests"]:
            host_tests.update(res["host_tests"])
        if res["data_deps"]:
            data_deps.update(res["data_deps"])
        if res["files_read"]:
            build_packages["files_read"].update(res["files_read"])

    monolith_targets = set(monolith_results["targets"] if monolith_results else ())
    preinstall_targets = set(preinstall_results["targets"] if preinstall_results else ())
    available_targets = set(available_results["targets"] if available_results else ())

    # preinstall_targets must not include monolith targets
    preinstall_targets -= monolith_targets

    # available_targets must include monolith and preinstall targets
    available_targets |= monolith_targets | preinstall_targets

    print(json.dumps({
        "monolith": list(monolith_targets),
        "preinstall": list(preinstall_targets),
        "available": list(available_targets),
        "host_tests": list(host_tests),
        "data_deps": list(data_deps),
        "files_read": list(build_packages["files_read"]),
    }))

    return 0

if __name__ == "__main__":
    sys.exit(main())
