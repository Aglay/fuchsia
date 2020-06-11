#!/usr/bin/env python2.7
# Copyright 2019 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import re
import subprocess
import time

from host import Host
from cli import CommandLineInterface


class Device(object):
    """Represents a Fuchsia device attached to a host.

       This class abstracts the details of remotely running commands and
       transferring data to and from the device.

       Attributes:
         host:             The associated Host object for this device.
         addr:             The device's IPv6 address.
         port:             TCP port number that sshd is listening on.
         ssh_config:       Path to an SSH configuration file.
         ssh_identity:     Path to an SSH identity file.
         ssh_options:      SSH options as in an SSH configuration file.
         ssh_verbosity:    How verbose SSH processes are, from 0 to 3.
    """

    def __init__(self, host, addr):
        assert host, 'Device host not set.'
        assert addr, 'Device address not set.'
        self._host = host
        self._addr = addr
        self._ssh_options = {}
        self._ssh_config_options = []
        self._ssh_verbosity = 0
        self._pids = None

    @property
    def host(self):
        """The associated Host object for this device."""
        return self._host

    @property
    def cli(self):
        """Alias for host.cli."""
        return self.host.cli

    @property
    def addr(self):
        """IPv6 address of the device."""
        return self._addr

    @property
    def port(self):
        """The TCP port number that the device's sshd is listening on."""
        return int(self._ssh_options.get('P', '22'))

    @port.setter
    def port(self, port):
        self._ssh_options['P'] = str(port)

    @property
    def ssh_config(self):
        """Path to an SSH configuration file."""
        return self._ssh_options.get('F', None)

    @ssh_config.setter
    def ssh_config(self, ssh_config):
        if not self.cli.isfile(ssh_config):
            raise ValueError(
                'Invalid SSH configuration file: {}'.format(ssh_config))
        self._ssh_options['F'] = ssh_config

    @property
    def ssh_identity(self):
        """Path to an SSH identity file."""
        return self._ssh_options.get('i', None)

    @ssh_identity.setter
    def ssh_identity(self, ssh_identity):
        if not self.cli.isfile(ssh_identity):
            raise ValueError(
                'Invalid SSH identity file: {}'.format(ssh_identity))
        self._ssh_options['i'] = ssh_identity

    @property
    def ssh_options(self):
        """SSH configuration options, as in an SSH configuration file."""
        return self._ssh_config_options

    @ssh_options.setter
    def ssh_options(self, ssh_options):
        self._ssh_config_options = ssh_options

    @property
    def ssh_verbosity(self):
        """How verbose SSH processes are, from 0 to 3."""
        return self._ssh_verbosity

    @ssh_verbosity.setter
    def ssh_verbosity(self, ssh_verbosity):
        if ssh_verbosity < 0 or ssh_verbosity > 3:
            raise ValueError('Invalid ssh_verbosity: {}'.format(ssh_verbosity))
        self._ssh_verbosity = ssh_verbosity

    def configure(self):
        """Sets the defaults for this device."""
        self.ssh_config = self.host.fxpath(
            self.host.build_dir, 'ssh-keys', 'ssh_config')

    def ssh_opts(self):
        """Returns the SSH executable and options."""
        ssh_options = []

        # Flags
        if self._ssh_verbosity != 0:
            ssh_options += ['-{}'.format('v' * self._ssh_verbosity)]

        # Options
        for key, val in sorted(self._ssh_options.iteritems()):
            ssh_options += ['-{}'.format(key), val]

        # Configuration options
        for val in sorted(self._ssh_config_options):
            ssh_options += ['-o', val]

        return ssh_options

    def ssh(self, args, **kwargs):
        """Creates a Process with added SSH arguments.

    Provides the additional arguments to handle connecting the device and other
    SSH options. The returned Process represents a command that can be run on
    the remote device.

    Args:
      cmdline: List of command line arguments to execute on device
      kwargs: Same as for subprocess.Popen

    Returns:
      A Process object.
    """
        args = ['ssh'] + self.ssh_opts() + [self.addr] + args
        p = self.cli.create_process(args, **kwargs)

        # Explicitly prevent the subprocess from inheriting our stdin
        if not p.stdin:
            p.stdin = CommandLineInterface.DEVNULL

        return p

    def getpid(self, package, executable, refresh=False):
        """Returns a process ID for a packaged executable if running, or -1.

           Relative to the duration of most "fx fuzz" commands, the SSH
           invocation to get component status is fairly expensive. Most
           prcoesses won't meaningfully change during or command (or will only
           change as a result of it). Thus, it makes sense to generally cache
           the PID results. This can lead to small degree of inaccuracy, e.g.
           "fx fuzz check" reporting a fuzzer as "RUNNING" when it stops
           between the command invocation and the display of results. This is
           unlikely (and inconseuqential) enough in normal operation to be
           deemed acceptable.

           If an accurate status is needed, e.g. as part of a long-lived command
           like Fuzzer.monitor(), "refresh" can be set to True to re0run the SSH
           command.
        """
        if self._pids == None or refresh:
            self._pids = {}
            out = self.ssh(['cs']).check_output()
            pat = re.compile(
                r'  (?P<executable>.*)\.cmx\[(?P<pid>\d+)\]: ' +
                r'fuchsia-pkg://fuchsia.com/(?P<package>.*)#meta/(?P=executable).cmx'
            )
            for line in str(out).split('\n'):
                match = re.match(pat, line)
                if not match:
                    continue
                groupdict = match.groupdict()
                nametuple = (groupdict['package'], groupdict['executable'])
                pid = int(groupdict['pid'])
                self._pids[nametuple] = pid
        return self._pids.get((package, executable), -1)

    def ls(self, path):
        """Returns a map of file names to sizes for the given path."""
        results = {}
        try:
            out = self.ssh(['ls', '-l', path]).check_output()
            for line in str(out).split('\n'):
                # Line ~= '-rw-r--r-- 1 0 0 8192 Mar 18 22:02 some-name'
                parts = line.split()
                if len(parts) > 8:
                    results[' '.join(parts[8:])] = int(parts[4])
        except subprocess.CalledProcessError:
            pass
        return results

    def mkdir(self, pathname):
        """Make a directory on the device."""
        cmd = ['mkdir', '-p', pathname]
        self.ssh(cmd).check_call()

    def remove(self, pathname, recursive=False):
        """Removes a file or directory from the device."""
        if recursive:
            cmd = ['rm', '-rf', pathname]
        else:
            cmd = ['rm', '-f', pathname]
        self.ssh(cmd).check_call()

    def dump_log(self, *args):
        """Retrieve a syslog from the device."""
        cmd = ['log_listener', '--dump_logs', 'yes', '--pretty', 'no'
              ] + list(args)
        return self.ssh(cmd).check_output()

    def guess_pid(self):
        """Tries to guess the fuzzer process ID from the device syslog.

        This will assume the last line which contained one of the strings
        '{{{reset}}}', 'libFuzzer', or 'Sanitizer' is the fuzzer process, and
        try to extract its PID.

        Returns:
          The PID of the process suspected to be the fuzzer, or -1 if no
          suitable candidate was found.
        """
        out = self.dump_log('--only', 'reset,Fuzzer,Sanitizer')
        pid = -1
        if out:
            for line in out.split('\n'):
                # Log lines are like '[timestamp][pid][tid][name] data'
                parts = line.split('][')
                if len(parts) > 2:
                    pid = int(parts[1])
        return pid

    def scp_rpath(self, pathname):
        """Returns an scp-style pathname argument for a remote path."""
        return '[{}]:{}'.format(self.addr, pathname)

    def fetch(self, host_dst, *args):
        """Copies files on the device to a directory on the host.

           The host directory is given by the first argument.

           This does not retry, even if the fetch is racing the file creation.
           In this case, the correct approach is to use some other signal to
           determine when the file(s) should be fetched. See Fuzzer._launch()
           for an example.
        """
        if not self.cli.isdir(host_dst):
            raise ValueError(host_dst + ' is not a directory')

        device_srcs = []
        for device_src in args:
            device_srcs.append(self.scp_rpath(device_src))

        cmd = ['scp'] + self.ssh_opts() + device_srcs + [host_dst]
        self.cli.create_process(cmd).check_call()

    def store(self, device_dst, *args):
        """Copies files on the host to a directory on the device.

           The device directory is given by the first argument.
        """
        self.mkdir(device_dst)
        device_dst = self.scp_rpath(device_dst)

        host_srcs = []
        for host_src in args:
            host_srcs += self.cli.glob(host_src)

        if host_srcs:
            cmd = ['scp'] + self.ssh_opts() + host_srcs + [device_dst]
            self.cli.create_process(cmd).check_call()
        return host_srcs
