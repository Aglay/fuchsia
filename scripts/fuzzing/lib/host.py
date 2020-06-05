#!/usr/bin/env python2.7
# Copyright 2019 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import errno
import glob
import json
import os
import re
import shutil
import subprocess

from process import Process


class Host(object):
    """Represents a local system with a build of Fuchsia.

    This class abstracts the details of various repository, tool, and build
    paths, as well as details about the host architecture and platform.

    Attributes:
      fuzzers:   The fuzzer binaries available in the current Fuchsia image
      build_dir: The build output directory, if present.
  """

    # Convenience file descriptor for silencing subprocess output
    DEVNULL = open(os.devnull, 'w')

    @classmethod
    def from_build(cls):
        """Uses a local build directory to configure a Host object from it."""
        fuchsia_dir = os.getenv('FUCHSIA_DIR')
        if not _fuchsia_dir:
            raise RuntimeError('FUCHSIA_DIR not set: have you `fx set`?')
        host = cls(fuchsia_dir)
        try:
            with open(host.fxpath('.fx-build-dir')) as opened:
                build_dir = opened.read().strip()
            with open(host.fxpath(build_dir, 'fuzzers.json')) as opened:
                host.configure(build_dir, opened)
        except IOError as e:
            if e.errno == errno.ENOENT:
                raise RuntimeError(
                    'Initialization failure; have you run ' +
                    '`fx set ... --fuzz-with <sanitizer>`?')
            else:
                raise
        return host

    def __init__(self, fuchsia_dir):
        self._platform = 'mac-x64' if os.uname()[0] == 'Darwin' else 'linux-x64'
        self._fuchsia_dir = fuchsia_dir
        self._build_dir = None
        self._symbolizer_exec = None
        self._llvm_symbolizer = None
        self._build_id_dirs = None
        self._fuzzers = []

    @property
    def fuchsia_dir(self):
        if not self._fuchsia_dir:
            raise RuntimeError('Fuchsia source tree location not set.')
        return self._fuchsia_dir

    @fuchsia_dir.setter
    def fuchsia_dir(self, fuchsia_dir):
        if not self.isdir(fuchsia_dir):
            raise ValueError('Invalid FUCHSIA_DIR: {}'.format(fuchsia_dir))
        self._fuchsia_dir = fuchsia_dir

    @property
    def build_dir(self):
        if not self._build_dir:
            raise RuntimeError('Build directory not set')
        return self._build_dir

    @property
    def build_id_dirs(self):
        if not self._build_id_dirs:
            raise RuntimeError('Build ID directories not set.')
        return self._build_id_dirs

    @build_id_dirs.setter
    def build_id_dirs(self, build_id_dirs):
        for build_id_dir in build_id_dirs:
            if not self.isdir(build_id_dir):
                raise ValueError(
                    'Invalid build ID directory: {}'.format(build_id_dir))
        self._build_id_dirs = build_id_dirs

    @property
    def symbolizer_exec(self):
        if not self._symbolizer_exec:
            raise RuntimeError('Symbolizer executable not set.')
        return self._symbolizer_exec

    @symbolizer_exec.setter
    def symbolizer_exec(self, symbolizer_exec):
        if not self.isfile(symbolizer_exec):
            raise ValueError(
                'Invalid symbolizer executable: {}'.format(symbolizer_exec))
        self._symbolizer_exec = symbolizer_exec

    @property
    def llvm_symbolizer(self):
        if not self._llvm_symbolizer:
            raise RuntimeError('LLVM symbolizer not set.')
        return self._llvm_symbolizer

    @llvm_symbolizer.setter
    def llvm_symbolizer(self, llvm_symbolizer):
        if not self.isfile(llvm_symbolizer):
            raise ValueError(
                'Invalid LLVM symbolizer: {}'.format(llvm_symbolizer))
        self._llvm_symbolizer = llvm_symbolizer

    @property
    def fuzzers(self):
        return self._fuzzers

    # Initialization routines

    def configure(self, build_dir, opened_fuzzers_json=None):
        """Sets multiple properties based on the given build directory."""
        self._build_dir = self.fxpath(build_dir)
        clang_dir = os.path.join(
            'prebuilt', 'third_party', 'clang', self._platform)
        self.symbolizer_exec = self.fxpath(build_dir, 'host_x64', 'symbolize')
        self.llvm_symbolizer = self.fxpath(clang_dir, 'bin', 'llvm-symbolizer')
        self.build_id_dirs = [
            self.fxpath(clang_dir, 'lib', 'debug', '.build-id'),
            self.fxpath(build_dir, '.build-id'),
            self.fxpath(build_dir + '.zircon', '.build-id'),
        ]
        if opened_fuzzers_json:
            self.read_fuzzers(opened_fuzzers_json)

    def read_fuzzers(self, open_file_obj):
        """Parses the available fuzzers from an open file object."""
        fuzz_specs = json.load(open_file_obj)
        for fuzz_spec in fuzz_specs:
            package = fuzz_spec['fuzzers_package']
            for executable in fuzz_spec['fuzzers']:
                self.fuzzers.append((package, executable))
        self.fuzzers.sort()

    # Filesystem routines.
    # These can be overriden during testing to remove dependencies on real files
    # and directories.

    def isdir(self, pathname):
        return os.path.isdir(pathname)

    def isfile(self, pathname):
        return os.path.isfile(pathname)

    def mkdir(self, pathname):
        try:
            os.makedirs(pathname)
        except OSError:
            if e.errno != errno.EEXIST:
                raise

    def rmdir(self, pathname):
        shutil.rmtree(pathname)

    def link(self, pathname, linkname):
        try:
            os.unlink(linkname)
        except OSError as e:
            if e.errno != errno.ENOENT:
                raise
        os.symlink(pathname, linkname)

    def glob(self, pattern):
        return glob.glob(pattern)

    # Subprocess routines.
    # These can be overriden during testing to remove dependencies on real files
    # and directories.

    def create_process(self, args, **kwargs):
        return Process(args, **kwargs)

    def killall(self, process):
        """ Invokes killall on the process name."""
        p = self.create_process(
            ['killall', process], stdout=Host.DEVNULL, stderr=Host.DEVNULL)
        p.call()

    # Other routines

    def fxpath(self, *segments):
        joined = os.path.join(*segments)
        if not joined.startswith(self.fuchsia_dir):
            joined = os.path.join(self.fuchsia_dir, joined)
        return joined

    def _find_device_cmd(self, device_name=None):
        cmd = [self.fxpath('.jiri_root', 'bin', 'fx'), 'device-finder']
        if device_name:
            cmd += ['resolve', '-device-limit', '1', device_name]
        else:
            cmd += ['list']
        return cmd

    def find_device(self, device_name=None, device_file=None):
        """Returns the IPv6 address for a device."""
        assert (not device_name or not device_file)
        if not device_name and device_file:
            device_name = device_file.read().strip()
        cmd = self._find_device_cmd(device_name)
        addrs = self.create_process(cmd).check_output().strip()
        if not addrs:
            raise RuntimeError('Unable to find device; try `fx set-device`.')
        addrs = addrs.split('\n')
        if len(addrs) != 1:
            raise RuntimeError('Multiple devices found; try `fx set-device`.')
        return addrs[0]

    def _symbolizer_cmd(self):
        cmd = [self.symbolizer_exec, '-llvm-symbolizer', self.llvm_symbolizer]
        for build_id_dir in self.build_id_dirs:
            cmd += ['-build-id-dir', build_id_dir]
        return cmd

    def symbolize(self, raw):
        """Symbolizes backtraces in a log file using the current build.

        Attributes:
            raw: Bytes representing unsymbolized lines.

        Returns:
            Bytes representing symbolized lines.
        """
        p = self.create_process(self._symbolizer_cmd())
        p.stdin = subprocess.PIPE
        p.stdout = subprocess.PIPE
        proc = p.popen()
        out, _ = proc.communicate(raw)
        if proc.returncode != 0:
            out = ''
        return re.sub(r'[0-9\[\]\.]*\[klog\] INFO: ', '', out)

    def snapshot(self):
        integration = self.fxpath('integration')
        p = self.create_process(
            ['git', 'rev-parse', 'HEAD'], stderr=Host.DEVNULL, cwd=integration)
        return p.check_output().strip()
