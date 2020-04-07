#!/usr/bin/env python2.7
# Copyright 2019 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import argparse
import re
import sys

from host import Host


class ArgParser:

    def __init__(self, description):
        """ Create an argument parser for the described command."""
        self._description = description
        self._label_allowed = False
        self._name_required = True
        self._reset()

    def require_name(self, required):
        """ Sets whether the command requires a 'name' argument."""
        self._name_required = required
        self._reset()

    def allow_label(self, allowed):
        """ Sets whether the command allows a 'label' argument."""
        self._label_allowed = allowed
        self._reset()

    def _reset(self):
        """ Rebuilds the underlying ArgumentParser. """
        self._parser = argparse.ArgumentParser(description=self._description)

        # Optional positional label implies a name is required.
        assert not self._label_allowed or self._name_required

        # Positional arguments
        name_help = (
            'Fuzzer name to match.  This can be part of the package and/or ' +
            'target name, e.g. "foo", "bar", and "foo/bar" all match ' +
            '"foo_package/bar_target".')
        if self._name_required:
            self._parser.add_argument('name', help=name_help)
        else:
            self._parser.add_argument('name', nargs='?', help=name_help)
        if self._label_allowed:
            self._parser.add_argument(
                'label',
                nargs='?',
                default='latest',
                help='Installs the labeled version from CIPD. "label" may be ' +
                'either a "ref" or a key:value "tag"  as described in ' +
                '`cipd help`. By default, corpora are uploaded with the ' +
                '"latest" ref and a tag of "integration:<git-revision>" ' +
                'corresponding to current revision of the //integration ' +
                'repository.')

        # Flags
        self._parser.add_argument(
            '--device',
            help='Name of device, only needed when multiple devices are present.'
        )
        self._parser.add_argument(
            '--foreground',
            action='store_true',
            help='Displays fuzzer output. Implied for \'repro\' and \'merge\'.')
        self._parser.add_argument(
            '--debug',
            action='store_true',
            help='If true, disable exception handling in libFuzzer.')
        self._parser.add_argument(
            '--no-cipd',
            action='store_true',
            help='Skip steps which involve transferring packages to or from CIPD'
        )
        self._parser.add_argument(
            '--output', help='Path under which to store results.')
        self._parser.add_argument(
            '--staging',
            help='Host directory to use for un/packing corpus bundles. Defaults '
            + 'to a temporary directory.')
        self._parser.add_argument(
            '--monitor', action='store_true', help=argparse.SUPPRESS)

    def parse_args(self, args=None):
        """ Parses arguments, all of which must be recognized. """
        return self._parser.parse_args(args)

    def parse_known_args(self, args=None):
        """ Transitional method; to be removed. """
        return self._parser.parse_known_args(args)

    def parse(self, args=None):
        """ Parses known and unknown arguments.

        This will distribute arguments into four categories:
        1. Arguments and flags described above are for this process.
        2. Arguments of the form "-key=val" are libFuzzer options.
        3. Remaining arguments before '--' are libFuzzer positional arguments.
        4. Arguments after '--'' are for the fuzzer subprocess.

        Standard argument parsing via `argparse.parse_known_args` can
        distinguish between categories 1 and 2-4, but to further separate them
        this method must do additional parsing.

        Args:
            A list of command line arguments. If None, sys.argv is used.

        Returns:
            A tuple consisting of:
                1. An argparse-populated namespace
                2. A dict of libFuzzer options mapping keys to values.
                3. A list of libFuzzer positional arguments.
                4. A list of fuzzer subprocess arguments.
        """
        pat = re.compile(r'-(\S+)=(.*)')
        other = []
        libfuzzer_opts = {}
        subprocess_args = []
        pass_to_subprocess = False
        if not args:
            args = sys.argv[1:]
        for arg in args:
            m = pat.match(arg)
            if pass_to_subprocess:
                subprocess_args.append(arg)
            elif arg == '--':
                pass_to_subprocess = True
            elif m:
                libfuzzer_opts[m.group(1)] = m.group(2)
            else:
                other.append(arg)
        args, libfuzzer_args = self._parser.parse_known_args(other)
        return args, libfuzzer_opts, libfuzzer_args, subprocess_args
