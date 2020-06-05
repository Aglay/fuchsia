#!/usr/bin/env python2.7
# Copyright 2019 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import fnmatch
import os
import subprocess

import test_env
from lib.host import Host

from process_fake import FakeProcess


class FakeHost(Host):

    def __init__(self, autoconfigure=True):
        ########
        # Variables specific to FakeHost

        # Set of paths whose existence is faked.
        fuchsia_dir = 'fuchsia_dir'
        self.pathnames = [fuchsia_dir]

        # Maps commands to responses.
        self.responses = {}

        # Response to return if command not found in self.responses.
        self.default_response = ''

        #  Maps commands to number of times the command should fail.
        self.failures = {}

        # List of commands whose execution has been faked.
        self.history = []

        ########
        # Initialize superclass and set defaults
        super(FakeHost, self).__init__(fuchsia_dir)
        self._platform = 'fake'
        self._fuzzers = []
        if autoconfigure:
            build_dir = 'build_dir'
            self.add_fake_pathnames(build_dir)
            self.configure(build_dir)
            self.add_fake_fuzzers()

    def add_fake_pathnames(self, build_dir):
        """Add additional default fake paths."""
        clang_dir = os.path.join('prebuilt', 'third_party', 'clang', 'fake')
        self.pathnames += [
            self.fxpath(build_dir),
            self.fxpath(build_dir, 'host_x64', 'symbolize'),
            self.fxpath(clang_dir, 'bin', 'llvm-symbolizer'),
            self.fxpath(clang_dir, 'lib', 'debug', '.build-id'),
            self.fxpath(build_dir, '.build-id'),
            self.fxpath(build_dir + '.zircon', '.build-id'),
            self.fxpath(build_dir + '.zircon', 'tools'),
        ]

    def add_fake_fuzzers(self):
        """Add additional defaults fake fuzzers."""
        self._fuzzers += [
            (u'fake-package1', u'fake-target1'),
            (u'fake-package1', u'fake-target2'),
            (u'fake-package1', u'fake-target3'),
            (u'fake-package2', u'fake-target1'),
            (u'fake-package2', u'fake-target11'),
            (u'fake-package2', u'an-extremely-verbose-target-name')
        ]

    def isdir(self, pathname):
        """Fake implementation overriding Host.isdir."""
        return pathname in self.pathnames or os.path.isdir(pathname)

    def isfile(self, pathname):
        """Fake implementation overriding Host.isfile."""
        return pathname in self.pathnames

    def mkdir(self, pathname):
        """Fake implementation overriding Host.mkdir."""
        self.pathnames.append(pathname)
        self.create_process(['mkdir', '-p', pathname])

    def rmdir(self, pathname):
        """Fake implementation overriding Host.rmdir."""
        self.pathnames.remove(pathname)
        self.create_process(['rm', '-rf', pathname])

    def link(self, pathname, linkname):
        """Fake implementation overriding Host.link."""
        if linkname not in self.pathnames:
            self.pathnames.append(linkname)
        self.create_process(['ln', '-s', pathname, linkname])

    def glob(self, pattern):
        """Fake implementation overriding Host.glob."""
        pathnames = []
        for pathname in self.pathnames:
            if fnmatch.fnmatch(pathname, pattern):
                pathnames.append(pathname)
        return pathnames

    def create_process(self, args, **kwargs):
        """Fake implementation overriding Host.create_process."""
        p = FakeProcess(self, args, **kwargs)
        joined_args = ' '.join(args)
        remaining = self.failures.get(joined_args, 0)
        if remaining:
            self.failures[joined_args] = remaining - 1
            raise subprocess.CalledProcessError(1, joined_args, None)
        if joined_args in self.responses:
            p.response = '\n'.join(self.responses[joined_args]) + '\n'
        else:
            p.response = self.default_response
        return p

    def as_input(self, line):
        """Returns history pattern for standard input."""
        return ' < ' + line

    def with_cwd(self, line, cwd):
        """Returns history pattern for current working directory."""
        return 'CWD={} {}'.format(cwd, line)
