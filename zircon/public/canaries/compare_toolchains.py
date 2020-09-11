#!/usr/bin/env python3
# Copyright 2020 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Compare the outputs of GN and ZN toolchains when building the same canary
targets. In case of difference, an error message will explain the issue, and
the content of OUTPUT_DIR can be manually reviewed by a human to look at
differences.

This scripts must be run from the top-level Fuchsia directory, it will work
as follows:

1) Create two directories (out/toolchains.gn and out/toolchains.zn by default)
   and create an args.gn in each one of them, to be parse by the Fuchsia and
   Zircon build respectively, directing the graph to the canary targets.

2) Invoke 'gn gen' on both directories to invoke the Fuchsia and Zircon
   builds, respectively.

3) For a specific set of GN/ZN toolchain pairs, compare the content of
   their respective toolchain.ninja file to verify that they use the same
   tool() definitions.

4) For each canary target, Invoke 'ninja -C <dir> -t commands <target>' to
   get the list of commands used to build the target, process the result
   slightly to account for differences between the two build systems,
   then compare the results.

5) Populate OUTPUT_DIR/{gn,zn}/<toolchain>/ directories with `rules.json`
   file that corresponds to the toolchain.ninja file for each toolchain,
   as well as `<target>.commands` containing a prettified list of commands
   for each target.

   This makes it easy to review differences manually, especially with a
   graphical tool, e.g. "meld OUTPUT_DIR/zn OUTPUT_DIR/gn" will give a
   very useful overview of the differences.
"""

import argparse
import difflib
import json
import os
import platform
import shutil
import subprocess
import sys

# The prefix of the two directories where ninja build files will be generated,
# related to $FUCHSIA_DIR/out/. This script will populate
# $FUCSHIA_DIR/out/$PREFIX.zn and $FUCHSIA_DIR/out/$PREFIX.gn and compare
# their content.

_DEFAULT_OUT_PREFIX = 'toolchains'

# The list of toolchains to compare. For now, this is a list of scopes
# with the following schema:
#
#  name: Name for this toolchain pair / comparison. Will be used
#    to create an $OUTPUT_DIR/{gn,zn}/$NAME directory where processed
#    outputs will be stored for human review.
#
#  variants: [optional] List of variants to apply.
#
#  gn.toolchain: GN-build toolchain label.
#  zn.toolchain: ZN-build toolchain label.
#
#  output_extension: [optional] Executable extension for this toolchain,
#    default is empty (no extension).
#
#  no_shared: [optional] Boolean set to True if these toolchains do not
#    support building shared libraries. Default is False.
#
_ALL_TOOLCHAINS = [
    {
        'name': 'bootloader_x64',
        'gn': {
            'toolchain': '//zircon/bootloader:efi_x64',
        },
        'zn': {
            'toolchain': '//bootloader:efi-x64-win-clang',
        },
        'output_extension': 'exe',
        'no_shared': True,
    },
    {
        'name': 'bootloader_arm64',
        'gn': {
            'toolchain': '//zircon/bootloader:efi_arm64',
        },
        'zn': {
            'toolchain': '//bootloader:efi-arm64-win-clang',
        },
        'output_extension': 'exe',
        'no_shared': True,
    },
    {
        'name': 'user.vdso_x64',
        'gn': {
            'toolchain': '//zircon/system/ulib/zircon:user.vdso_x64',
        },
        'zn': {
            'toolchain': '//system/ulib/zircon:user.vdso-x64-clang',
        },
    },
    {
        'name': 'user.vdso_x64-gcc',
        'variants': ['gcc'],
        'gn': {
            'toolchain': '//zircon/system/ulib/zircon:user.vdso_x64-gcc',
        },
        'zn': {
            'toolchain': '//system/ulib/zircon:user.vdso-x64-gcc',
        },
    },
    {
        'name': 'user.vdso_arm64',
        'gn': {
            'toolchain': '//zircon/system/ulib/zircon:user.vdso_arm64',
        },
        'zn': {
            'toolchain': '//system/ulib/zircon:user.vdso-arm64-clang',
        },
    },
    {
        'name': 'user.vdso_arm64-gcc',
        'variants': ['gcc'],
        'gn': {
            'toolchain': '//zircon/system/ulib/zircon:user.vdso_arm64-gcc',
        },
        'zn': {
            'toolchain': '//system/ulib/zircon:user.vdso-arm64-gcc',
        },
    },
    {
        'name': 'multiboot',
        'gn':
            {
                'toolchain':
                    '//zircon/kernel/target/pc/multiboot:zircon_multiboot',
            },
        'zn': {
            'toolchain': '//kernel/target/pc/multiboot:multiboot-x64-clang',
        },
        'no_shared': True,
    },
    {
        'name': 'multiboot-gcc',
        'variants': ['gcc'],
        'gn':
            {
                'toolchain':
                    '//zircon/kernel/target/pc/multiboot:zircon_multiboot-gcc',
            },
        'zn': {
            'toolchain': '//kernel/target/pc/multiboot:multiboot-x64-gcc',
        },
        'no_shared': True,
    },
    {
        'name': 'physmem_arm64',
        'gn': {
            'toolchain': '//zircon/kernel/arch/arm64:physmem_arm64',
        },
        'zn': {
            'toolchain': '//kernel/arch/arm64:physmem-arm64-clang',
        },
        'no_shared': True,
    },
    {
        'name': 'physmem_arm64-gcc',
        'variants': ['gcc'],
        'gn': {
            'toolchain': '//zircon/kernel/arch/arm64:physmem_arm64-gcc',
        },
        'zn': {
            'toolchain': '//kernel/arch/arm64:physmem-arm64-gcc',
        },
        'no_shared': True,
    },
    {
        'name': 'kernel.phys32',
        'gn': {
            'toolchain': '//zircon/kernel/arch/x86/phys:kernel.phys32',
        },
        'zn': {
            'toolchain': '//kernel/arch/x86/phys:kernel.phys32-x64-clang',
        },
        'no_shared': True,
    },
    {
        'name': 'kernel.phys32-gcc',
        'variants': ['gcc'],
        'gn': {
            'toolchain': '//zircon/kernel/arch/x86/phys:kernel.phys32-gcc',
        },
        'zn': {
            'toolchain': '//kernel/arch/x86/phys:kernel.phys32-x64-gcc',
        },
        'no_shared': True,
    },
    {
        'name': 'kernel.phys-x64',
        'gn': {
            'toolchain': '//zircon/kernel/phys:kernel.phys_x64',
        },
        'zn': {
            'toolchain': '//kernel/phys:kernel.phys-x64-clang',
        },
        'no_shared': True,
    },
    {
        'name': 'kernel.phys-arm64',
        'gn': {
            'toolchain': '//zircon/kernel/phys:kernel.phys_arm64',
        },
        'zn': {
            'toolchain': '//kernel/phys:kernel.phys-arm64-clang',
        },
        'no_shared': True,
    },
    {
        'name': 'kernel.phys-x64-gcc',
        'variants': ['gcc'],
        'gn': {
            'toolchain': '//zircon/kernel/phys:kernel.phys_x64-gcc',
        },
        'zn': {
            'toolchain': '//kernel/phys:kernel.phys-x64-gcc',
        },
        'no_shared': True,
    },
    {
        'name': 'kernel.phys-arm64-gcc',
        'variants': ['gcc'],
        'gn': {
            'toolchain': '//zircon/kernel/phys:kernel.phys_arm64-gcc',
        },
        'zn': {
            'toolchain': '//kernel/phys:kernel.phys-arm64-gcc',
        },
        'no_shared': True,
    },
    {
        'name': 'userboot_arm64',
        'gn':
            {
                'toolchain':
                    '//zircon/kernel/lib/userabi/userboot:userboot_arm64',
            },
        'zn':
            {
                'toolchain':
                    '//kernel/lib/userabi/userboot:userboot-arm64-clang',
            },
        'no_shared': True,
    },
    {
        'name': 'userboot_x64',
        'gn':
            {
                'toolchain':
                    '//zircon/kernel/lib/userabi/userboot:userboot_x64',
            },
        'zn': {
            'toolchain': '//kernel/lib/userabi/userboot:userboot-x64-clang',
        },
        'no_shared': True,
    },
    {
        'name': 'userboot_arm64-gcc',
        'variants': ['gcc'],
        'gn':
            {
                'toolchain':
                    '//zircon/kernel/lib/userabi/userboot:userboot_arm64-gcc',
            },
        'zn': {
            'toolchain': '//kernel/lib/userabi/userboot:userboot-arm64-gcc',
        },
        'no_shared': True,
    },
    {
        'name': 'userboot_x64-gcc',
        'variants': ['gcc'],
        'gn':
            {
                'toolchain':
                    '//zircon/kernel/lib/userabi/userboot:userboot_x64-gcc',
            },
        'zn': {
            'toolchain': '//kernel/lib/userabi/userboot:userboot-x64-gcc',
        },
        'no_shared': True,
    },
]

_TOOLCHAIN_NAMES = [e['name'] for e in _ALL_TOOLCHAINS]

# The list of GN tool names that needs to be compared. The others are ignored
# by this script (e.g. Objective-C, Rust and Copy + Stamp).
_COMMON_TOOLS = {'alink', 'link', 'asm', 'cc', 'cxx', 'solink', 'solink_module'}

# The Clang binary directory, as it should appear in build commands.
_CLANG_BINPREFIX = '../../prebuilt/third_party/clang/linux-x64/bin'


def _entry_to_variant_name(e):
    """Return the variant-specific name for entry |e| in _ALL_TOOLCHAINS."""
    return '-'.join(e.get('variants', []))


def _recreate_directory(dir_path):
    """Create or cleanup |dir_path| directory path."""
    if os.path.isdir(dir_path):
        shutil.rmtree(dir_path)
    os.makedirs(dir_path)


def _write_file(path, data):
    """Write |data| to file |path|."""
    with open(path, 'w') as f:
        f.write(data)


def _cmd_output(cmds):
    """Return shell command output as a string."""
    return subprocess.check_output(cmds).decode('utf-8').rstrip()


def _write_dict_as_json(json_file, rules):
    """Write dictionary as a JSON file."""
    with open(json_file, 'w') as f:
        json.dump(rules, f, indent=2, sort_keys=True)


def _generate_gn_args_for_gn_build(toolchains, variant_name):
    """Generate the args.gn file used by the GN canary build."""
    result = '# Auto-generated - DO NOT EDIT\n'
    result += 'base_package_labels = []\n'
    result += 'cache_package_labels = []\n'
    result += 'universe_package_labels = [\n'

    for t in toolchains:
        if variant_name == _entry_to_variant_name(t):
            result += '  "//zircon/public/canaries:canaries(%s)",' % t['gn'][
                'toolchain']
    result += ']\n'
    if variant_name:
        variants = variant_name.split('-')
        result += 'select_variant = [ %s ]\n' % ', '.join(
            '"%s"' % v for v in variants)
    return result


def _generate_gn_args_for_zn_build(toolchains, variant_name):
    """Generate the args.gn file used by the ZN canary build."""
    result = '# Auto-generated - DO NOT EDIT\n'
    result += "default_deps = [\n"
    for t in toolchains:
        if variant_name == _entry_to_variant_name(t):
            result += '  "//public/canaries:canaries(%s)",' % t['zn'][
                'toolchain']
    result += "]\n"
    if variant_name:
        variants = variant_name.split('-')
        result += 'variants = [ %s ]\n' % ', '.join(
            '"%s"' % v for v in variants)
    return result


def parse_toolchain_ninja_file(toolchain_ninja, toolchain):
    """Parse the content of a toolchain.ninja file.

    Parse the content of a toolchain.ninja file, and extract its tool-specific
    entries (e.g. ${toolchain}_alink, ${toolchain}_link, etc).
    Ignoring the rest.

    Args:
        toolchain_ninja: A string holding the toolchain.ninja file content.
        toolchain: The toolchain's name, will be replaced by 'TOOLCHAIN'
          in the output.

    Returns:
        A { tool_name -> tool_dict } dictionary, where |tool_name| is a GN
        tool() name, and tool_dict() is a dictionary of the corresponding
        entries from the input data, with "${toolchain}" replaced with
        'TOOLCHAIN'. E.g.:

            {
              "alink": {
                "command": "......",
                "description": "....",
                "rspfile": "....",
                ...
              },
              ...
            }
    """
    content = toolchain_ninja.replace(toolchain, 'TOOLCHAIN')
    tool_name = None
    tool_rule_prefix = 'rule TOOLCHAIN_'
    result = {}

    for line in content.splitlines():
        line = line.rstrip()
        if not line:
            # Empty lines exit the tool section.
            tool_name = None
            continue

        if line[0] != ' ':
            # Expected format for the start of a tool section:
            # rule ${toolchain}_${tool}.
            if line.startswith(tool_rule_prefix):
                tool_name = line[len(tool_rule_prefix):]
                result[tool_name] = {}
                continue
        elif tool_name:
            # Expected format for a tool section line:
            # <key> = <value>
            key, separator, value = line.partition('=')
            assert separator == '=', 'Unknown tool section line: ' + line
            key = key.strip()
            value = value.strip()
            result[tool_name][key] = value
            continue

        # Something else, ignore and exit tool section if any.
        tool_name = None

    return result


def _remove_unnecessary_tools(rules):
    """Remove any un-needed tools from a set of toolchain rules.

    Args:
        rules: A { tool name -> tool dict } dictionary as returned by
          parse_toolchain_ninja_file().
    Returns:
        The input dictionary, but only with the keys from _COMMON_TOOLS.
    """
    return {k: v for k, v in rules.items() if k in _COMMON_TOOLS}


def _load_toolchain_ninja(out_dir, toolchain_label):
    """Load a toolchain.ninja path for a given toolchain.

    Args:
      out_dir: Root output directory (e.g. "$FUCHSIA_DIR/out/toolchains.gn")
      toolchain: Toolchain GN label (e.g. "//build/toolchain:host_x64").

    Returns:
      A (content, toolchain) tuple, where |content| is a string containing
      the file's content, and |toolchain| is the toolchain's name
      (e.g. "host_x64").
    """
    _, _, toolchain_name = toolchain_label.partition(':')
    toolchain_ninja_path = os.path.join(
        out_dir, toolchain_name, 'toolchain.ninja')
    with open(toolchain_ninja_path, 'r') as f:
        toolchain_ninja = f.read()
    return (toolchain_ninja, toolchain_name)


def pretty_print_commands_list(commands):
    """Convert a list of commands into a pretty-printed string."""
    result = ""
    for cmd in commands:
        cmd0, _, _ = cmd.partition(' ')
        is_compile_or_link = (
            cmd0.endswith('clang') or cmd0.endswith('clang++') or
            cmd0.endswith('g++') or cmd0.endswith('gcc'))

        if is_compile_or_link:
            # This is a compiler/linker command, use one argument per line.
            cmd = ' \\\n    '.join(cmd.split(' '))
        else:
            # This is a regular command, split at && instead.
            cmd = '&& \\\n   '.join(cmd.split('&&'))

        result += cmd + '\n'

    return result


def _write_commands_to_file(path, commands):
    """Write target commands to a file, pretty-printing it to help comparison."""
    with open(path, 'w') as f:
        f.write(pretty_print_commands_list(commands))


def _remove_gn_config_deps_touch_commands(commands):
    """Remove extra stamp touch commands from GN build.

    The Fuchsia build adds stamps for config_deps groups, remove
    them since they are not important for this comparison.
    They look like:
       touch TOOLCHAIN/obj/.../${config}_deps.stamp

    Args:
      commands: List of command strings from the GN build.
    Returns:
      A new list of command strings.
    """
    return [
        cmd for cmd in commands if not (
            cmd.startswith('touch TOOLCHAIN/') and cmd.endswith('_deps.stamp'))
    ]


def _update_gn_executable_output_directory(commands):
    """Update the output path of executables and response files.

    The GN and ZN builds place their executables in different locations
    so adjust then GN ones to match the ZN ones.

    Args:
      commands: list of command strings from the GN build.
    Returns:
      A new list of command strings.
    """
    replacements = {
        'TOOLCHAIN/main_with_static':
            'TOOLCHAIN/obj/public/canaries/main_with_static',
        'TOOLCHAIN/main_with_shared':
            'TOOLCHAIN/obj/public/canaries/main_with_shared',
        'TOOLCHAIN_SHARED/libfoo_shared':
            'TOOLCHAIN_SHARED/obj/public/canaries/libfoo_shared',
    }

    result = []
    for cmd in commands:
        for key, val in replacements.items():
            cmd = cmd.replace(key, val)
        result.append(cmd)

    return result


def _update_root_build_dir(commands, root_build_dir):
    """Update the GN root build dir when it appears in commands.

    The absolute root build dir sometimes appears in build commands (e.g.
    when building with a 'gcc' variant, in a -fdebug-prefix=<...> compiler
    argument). This function replaces it with the hard-coded ROOT_BUILD_DIR
    string.

    Args:
      commands: list of command strings from either build.
      root_build_dir: root build dir path.
    Returns:
      A new list of command strings.
    """
    root_build_dir = os.path.abspath(root_build_dir)
    return [cmd.replace(root_build_dir, 'ROOT_BUILD_DIR') for cmd in commands]


def _update_zn_prebuilt_path(commands, root_dir):
    """Update the absolute prebuilt directory when it appears in ZN commands.

    The absolute root prebuilt dir sometimes appears in ZN build commands, e.g.
    when using a 'gcc' variant, it will appear in a compiler argument
    that lists the path to the libc++ library. Meanwhile in the GN build, the
    corresponding relative path (i.e. '../../prebuilt') will be used instead.

    The reason for this is that GN will change the value of 'lib_dirs' from
    absolute to relative when writing Ninja toolchain definitions, if the path
    is absolute *and* in-tree.

    In the case of the GN build, prebuilt is under the root directory, so the
    substitution always happen (and cannot be disabled). In the case of the
    ZN build, whose root_dir is really FUCHSIA_DIR/zircon/, the prebuilt
    directory is out-of-tree so the value written to toolchain.ninja will still
    be absolute for the same compiler argument.

    This function deals with the issue by replacing the absolute prebuilt
    directory path with '../../prebuilt' string, to match the GN build.
    """
    prebuilt_dir = os.path.abspath(os.path.join(root_dir, 'prebuilt'))
    return [cmd.replace(prebuilt_dir, '../../prebuilt') for cmd in commands]


def _check_toolchain_rules(
        gn_rules, gn_toolchain, zn_rules, zn_toolchain, verbose):
    """Check the toolchain rules between a GN toolchain and its ZN equivalent.

    Args:
      gn_rules: A { tool -> { key: value } } map, as returned by
        parse_toolchain_ninja_file(), corresponding to a GN toolchain.
      gn_toolchain: The GN toolchain label.
      zn_rules: Same as |gn_rules|, for the corresponding ZN toolchain.
      zn_toolchain: The ZN toolchain label.
      verbose: If True, add key value differences to error messages.

    Returns:
      A (warnings, errors) pair, where |warnings| and |errors| are list of
      strings describing warnings or errors found in the comparison.
    """

    warnings = []
    errors = []

    # Check that all tools in the ZN toolchain are in the GN one.
    zn_tools = set(zn_rules.keys())
    gn_tools = set(gn_rules.keys())
    missing_tools = zn_tools - gn_tools
    if missing_tools:
        message = 'Missing tools from GN:%s (compared with ZN:%s): %s' % (
            gn_toolchain, zn_toolchain, ' '.join(sorted(missing_tools)))
        errors += [message]

    # For each tool, check that the keys are the same.
    # Then check that their values are also identical.
    for tool in zn_tools:
        zn_tool = zn_rules[tool]
        gn_tool = gn_rules[tool]

        gn_keys = set(gn_tool.keys())
        zn_keys = set(zn_tool.keys())
        extra_keys = gn_keys - zn_keys
        if extra_keys:
            warnings += [
                'GN:%s: extra %s tool keys: %s' %
                (gn_toolchain, tool, sorted(extra_keys))
            ]

        missing_keys = zn_keys - gn_keys
        if missing_keys:
            errors += [
                'GN:%s: Missing %s tool keys: %s' %
                (gn_toolchain, tool, sorted(missing_keys))
            ]

        for key in gn_keys & zn_keys:
            zn_value = zn_tool[key]
            gn_value = gn_tool[key]

            if zn_value != gn_value:
                message = 'GN:%s:%s: %s key values differ' % (
                    gn_toolchain, tool, key)
                if verbose:
                    message += '\n  gn [%s]\n  zn [%s]\n' % (gn_value, zn_value)
                errors += [message]

    return warnings, errors


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument(
        'output_dir',
        metavar='OUTPUT_DIR',
        help=(
            'Output directory where processed toolchain rules and commands ' +
            'will be written for human review and comparison. Must not exist ' +
            'unless --clean is used.'))
    parser.add_argument(
        '--clean',
        action='store_true',
        help=(
            'Cleanup output directory if it already exists, instead of ' +
            'aborting.'))
    parser.add_argument(
        '--verbose',
        action='store_true',
        help='Print differences with error messages.')
    parser.add_argument(
        '--toolchain',
        help=
        'Comma-separated list of toolchains to check. Default is all of them')
    parser.add_argument(
        '--root-dir',
        help='Root Fuchsia source directory. Default is auto-detected.')
    parser.add_argument(
        '--gn',
        help='Path to GN executable. The prebuilt Fuchsia one will be used by '
        'default.')
    parser.add_argument(
        '--ninja',
        help='Path to Ninja executable. The prebuilt Fuchsia one will be used '
        'by default.')
    parser.add_argument(
        '--out-prefix',
        default=_DEFAULT_OUT_PREFIX,
        help='Prefix of output directories used for comparison, relative to ' +
        '$ROOT_DIR/out/. Default is [%s]' % _DEFAULT_OUT_PREFIX)
    parser.add_argument(
        '--skip-gen',
        action='store_true',
        help='Skip \'gn gen\' step. Only used for developing this script.')

    args = parser.parse_args()
    if args.root_dir is None:
        # Assume this script os under //zircon/public/canaries/
        root_dir = os.path.abspath(
            os.path.join(os.path.dirname(__file__), '..', '..', '..'))
    else:
        root_dir = args.root_dir

    assert os.path.isdir(root_dir), 'Missing root directory: ' + root_dir
    root_dir = os.path.abspath(root_dir)

    # Create or cleanup output directory if needed.
    output_dir = os.path.abspath(args.output_dir)
    if os.path.isdir(output_dir):
        if not args.clean:
            print(
                'ERROR: Output directory already exists. Consider using ' +
                '--clean to remove its content and use it.',
                file=sys.stderr)
            return 1
        _recreate_directory(output_dir)
    else:
        os.makedirs(output_dir)

    host_cpu = platform.machine()
    host_cpu = {
        'x86_64': 'x64',
        'aarch64': 'arm64',
    }.get(host_cpu, host_cpu)

    host_os = platform.system()
    host_os = {
        'Linux': 'linux',
        'Darwin': 'mac',
        'Windows': 'win',
    }.get(host_os, host_os)

    host_name = '%s-%s' % (host_os, host_cpu)

    if args.gn:
        gn_path = args.gn
    else:
        gn_path = os.path.join(
            root_dir, 'prebuilt', 'third_party', 'gn', host_name, 'gn')

    if args.ninja:
        ninja_path = args.ninja
    else:
        ninja_path = os.path.join(
            root_dir, 'prebuilt', 'third_party', 'ninja', host_name, 'ninja')

    all_toolchains = _ALL_TOOLCHAINS
    if args.toolchain:
        toolchain_names = set(args.toolchain.split(','))
        for name in toolchain_names:
            if name not in _TOOLCHAIN_NAMES:
                parser.error(
                    'Invalid toolchain name %s, should be one of:\n%s\n' %
                    (name, '\n'.join('  %s' % t for t in _TOOLCHAIN_NAMES)))
        all_toolchains = [
            t for t in _ALL_TOOLCHAINS if t['name'] in toolchain_names
        ]

    all_variant_names = sorted(
        {_entry_to_variant_name(e) for e in all_toolchains})

    # Generate all GN and ZN output directories, based on the variants being used.
    gn_out_dirs = {}
    zn_out_dirs = {}

    for variant in all_variant_names:
        suffix = '-' + variant if variant else ''
        gn_out_dir = os.path.join(
            root_dir, 'out', args.out_prefix + suffix + '.gn')
        zn_out_dir = os.path.join(
            root_dir, 'out', args.out_prefix + suffix + '.zn')

        gn_out_dirs[variant] = gn_out_dir
        zn_out_dirs[variant] = zn_out_dir

        if not args.skip_gen:
            # Generate the $ROOT_DIR/out/$PREFIX.gn/args.gn
            _recreate_directory(gn_out_dir)
            _write_file(
                os.path.join(gn_out_dir, 'args.gn'),
                _generate_gn_args_for_gn_build(all_toolchains, variant))

            # Populate $ROOT_DIR/out/$PREFIX.gn now.
            subprocess.check_call(
                [gn_path, 'gen',
                 os.path.relpath(gn_out_dir, start=root_dir)],
                cwd=root_dir)

            # Generate $ROOT_DIR/out/$PREFIX.zn/args.gn
            _recreate_directory(zn_out_dir)
            _write_file(
                os.path.join(zn_out_dir, 'args.gn'),
                _generate_gn_args_for_zn_build(all_toolchains, variant))

            # Populate $ROOT_DIR/out/$PREFIX.zn now
            subprocess.check_call(
                [
                    gn_path, 'gen', '--root=zircon',
                    os.path.relpath(zn_out_dir, start=root_dir)
                ],
                cwd=root_dir)

    for t in all_toolchains:
        gn_toolchain = t['gn']['toolchain']
        zn_toolchain = t['zn']['toolchain']

        variant = _entry_to_variant_name(t)
        variant_suffix = '-' + variant if variant else ''

        gn_out_dir = gn_out_dirs[variant]
        zn_out_dir = zn_out_dirs[variant]

        gn_toolchain_ninja, gn_toolchain_name = _load_toolchain_ninja(
            gn_out_dir, gn_toolchain)

        gn_toolchain_rules = parse_toolchain_ninja_file(
            gn_toolchain_ninja, gn_toolchain_name)

        zn_toolchain_ninja, zn_toolchain_name = _load_toolchain_ninja(
            zn_out_dir, zn_toolchain)

        zn_toolchain_rules = parse_toolchain_ninja_file(
            zn_toolchain_ninja, zn_toolchain_name)

        gn_toolchain_rules = _remove_unnecessary_tools(gn_toolchain_rules)
        zn_toolchain_rules = _remove_unnecessary_tools(zn_toolchain_rules)

        # All outputs for this toolchain pair will be in this directory.
        gn_output_dir = os.path.join(output_dir, 'gn', t['name'])
        zn_output_dir = os.path.join(output_dir, 'zn', t['name'])
        os.makedirs(gn_output_dir)
        os.makedirs(zn_output_dir)

        _write_dict_as_json(
            os.path.join(gn_output_dir, 'rules.json'), gn_toolchain_rules)

        _write_dict_as_json(
            os.path.join(zn_output_dir, 'rules.json'), zn_toolchain_rules)

        warnings, errors = _check_toolchain_rules(
            gn_toolchain_rules, gn_toolchain, zn_toolchain_rules, zn_toolchain,
            args.verbose)

        for w in warnings:
            print('WARNING: %s' % w, file=sys.stderr)

        # Compare output commands for canary targets now.
        targets = ['main_with_static']
        if not t.get('no_shared', False):
            targets += ['main_with_shared']

        extension = ""
        if 'output_extension' in t:
            extension = "." + t['output_extension']

        for target in targets:
            # The output directories are derived from the toolchain name.
            gn_outdir = gn_toolchain_name
            zn_outdir = zn_toolchain_name + '/obj/public/canaries'

            gn_outfile = os.path.join(gn_outdir, target) + extension
            zn_outfile = os.path.join(zn_outdir, target) + extension
            print(
                '%s: Comparing commands for GN [%s] and ZN [%s]' %
                (t['name'], gn_outfile, zn_outfile))

            gn_commands = _cmd_output(
                [ninja_path, '-C', gn_out_dir, '-t', 'commands', gn_outfile])

            zn_commands = _cmd_output(
                [ninja_path, '-C', zn_out_dir, '-t', 'commands', zn_outfile])

            # Replace toolchain name with TOOLCHAIN
            gn_commands = gn_commands.replace(
                gn_toolchain_name + '/', 'TOOLCHAIN/')
            zn_commands = zn_commands.replace(
                zn_toolchain_name + '/', 'TOOLCHAIN/')

            is_shared = target.endswith('shared')
            if is_shared:
                gn_commands = gn_commands.replace(
                    gn_toolchain_name + '-shared/', 'TOOLCHAIN_SHARED/')
                zn_commands = zn_commands.replace(
                    zn_toolchain_name + '.shlib/', 'TOOLCHAIN_SHARED/')

            # Build configurations come from //public/gn/config in the ZN build
            # and from //build/config/zircon in the GN one. These path can
            # appear in commands in certain cases.
            gn_commands = gn_commands.replace(
                'build/config/zircon/', 'BUILD_CONFIGS/')
            zn_commands = zn_commands.replace(
                'zircon/public/gn/config/', 'BUILD_CONFIGS/')

            # Remove /zircon/ sub-path from GN build commands.
            gn_commands = gn_commands.replace('/obj/zircon/', '/obj/').replace(
                '/gen/zircon/', '/gen/')

            # Replace out/toolchain.{gn,zn} with BUILD_ROOT_DIR
            gn_commands = gn_commands.replace(
                'out/%s.gn' % args.out_prefix, 'BUILD_ROOT_DIR')
            zn_commands = zn_commands.replace(
                'out/%s.zn' % args.out_prefix, 'BUILD_ROOT_DIR')

            # Split lines.
            gn_commands = gn_commands.splitlines()
            zn_commands = zn_commands.splitlines()

            # Sanitize GN commands a little.
            gn_commands = _remove_gn_config_deps_touch_commands(gn_commands)
            gn_commands = _update_gn_executable_output_directory(gn_commands)
            gn_commands = _update_root_build_dir(gn_commands, gn_out_dir)
            zn_commands = _update_root_build_dir(zn_commands, zn_out_dir)
            zn_commands = _update_zn_prebuilt_path(zn_commands, root_dir)

            if len(gn_commands) != len(zn_commands):
                errors += [
                    '%s: GN(%s) vs ZN(%s) line count mismatch: %d vs %d' % (
                        target, gn_toolchain_name, zn_toolchain_name,
                        len(gn_commands), len(zn_commands))
                ]

            # Compare commands list.
            differences = list(
                difflib.unified_diff(gn_commands, zn_commands, n=2))
            if len(differences) > 0:
                message = (
                    '%s: GN(%s) vs ZN(%s) have different content!' %
                    (target, gn_toolchain_name, zn_toolchain_name))
                if args.verbose:
                    message += '\n  %s' % '\n  '.join(differences)

                errors += [message]

            _write_commands_to_file(
                os.path.join(gn_output_dir, '%s.commands' % target),
                gn_commands)

            _write_commands_to_file(
                os.path.join(zn_output_dir, '%s.commands' % target),
                zn_commands)

    if errors:
        for e in errors:
            print('ERROR: %s' % e, file=sys.stderr)
        print('For full details, see the content of: %s' % output_dir)
        return 1

    print('OK.')
    return 0


if __name__ == "__main__":
    sys.exit(main())
