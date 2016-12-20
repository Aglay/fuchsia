#!/usr/bin/env bash
# Copyright 2016 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# This script wraps imager.py and provides some configuration convenience
# functionality. For example if a directory containing the fuchsia build output
# is not supplied we assume it is two directories up from the build script and
# then in a sub-directory for a given architecture. We also set sensible
# defaults for things like partition size, etc.

set -e -u

# construct the path to our directory
script_name=$(basename "$0")
script_dir=$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")
expected_path="scripts/installer"
curr_dir=$(pwd)

# check that it looks like the script lives inside a fuchsia source tree
if [ $(dirname "$(dirname "$script_dir")")/"$expected_path" != "$script_dir" ]; then
  echo "It doesn't look like we're running in the right place, please make" \
    "sure the script is in a fuchsia source tree in scripts/installer"
  exit -1
fi

DEFAULT_SIZE_SYSTEM=4
DEFAULT_SIZE_EFI=1
BLOCK_SIZE=1024
STAGING_DIR="${script_dir}/build-installer"
# TODO take a size for the magenta partition as well
blocks_sys=0
blocks_efi=0
release=0
debug=0
platform=""
build_dir_fuchsia=""
minfs_path=""
build_dir_magenta=""
device_type="pc"


while getopts ":u:hrdp:b:m:e:a:t:" opt; do
  case $opt in
    u)
      blocks_sys=$(($OPTARG * 1024 * 1024))
      ;;
    h)
      echo "build-installable-usersfs.sh -u <SIZE> [-r|-d] [-p] [-b <BUILD DIR>]"
      echo "-u: size of system partition in GB"
      echo "-e: size of the EFI partition in GB"
      echo "-r: use the release build directory, should not be used with -d"
      echo "-d: use the debug build directory, should not be used with -r"
      echo "-p: platform architecture, eg. x86-64, arm, or arm-64"
      echo "-b: specify the build directory manually, this will cause -r and" \
        "-d arguments to be ignored"
      echo "-m: path to the host architecture minfs binary, perhaps you need" \
        "to run 'make' in magenta/system/uapp/minfs"
      echo "-a: artifacts directory for magenta, will be used to find files" \
        "to place on the EFI partition. If not supplied, this will be assumed" \
        "relative to fuchsia build directory."
      echo "-t: the device type, for example 'qemu', 'rpi', 'pc', etc"
      exit 0
      ;;
    r)
      release=1
      ;;
    d)
      debug=1
      ;;
    p)
      platform=$OPTARG
      ;;
    b)
      build_dir_fuchsia=$OPTARG
      ;;
    m)
      minfs_path=$OPTARG
      ;;
    e)
      blocks_efi=$(($OPTARG * 1024 * 1024))
      ;;
    a)
      build_dir_magenta=$OPTARG
      ;;
    t)
      device_type=$OPTARG
      ;;
    \?)
      echo "Unknown option -$OPTARG"
  esac
done

if [ "$blocks_sys" -eq 0 ]; then
  blocks_sys=$(($DEFAULT_SIZE_SYSTEM * 1024 * 1024))
fi

if [ "$blocks_efi" -eq 0 ]; then
  blocks_efi=$(($DEFAULT_SIZE_EFI * 1024 * 1024))
fi

if [ "$build_dir_fuchsia" = "" ] || [ "$build_dir_magenta" = ""]; then
  if [ "$release" -eq "$debug" ]; then
    if [ "$debug" -eq 0 ]; then
      debug=1
    else
      echo "Please choose release or debug, but not both"
      exit -1
    fi
  fi

  if [ "$platform" = "" ]; then
    platform=x86-64
  fi

  if [ "$release" -eq 1 ]; then
    build_variant="release"
  else
    build_variant="debug"
  fi
fi

arch=""
case $platform in
  x86-64)
    arch="X64"
    ;;
  arm)
    arch="ARM"
    ;;
  arm-64)
    arch="AA64"
    ;;
  \?)
    echo "Platform is not valid, should be x86-64, arm, or arm-64!"
esac

# if the build directory is not specified, infer it from other parameters
if [ "$build_dir_fuchsia" = "" ]; then
  build_dir_fuchsia=$script_dir/../../out/$build_variant-$platform
else
  if [ "$release" -ne 0 ] || [ "$debug" -ne 0 ]; then
    echo "build directory is specified release arg ignored"
  fi
fi

if [ "$build_dir_magenta" = "" ]; then
  build_dir_magenta=$build_dir_fuchsia/../build-magenta/build-magenta-$device_type-$platform
else
  if [ "$device_type" -ne "" ]; then
    echo "build directory is specified, type arg ignored"
  fi
fi

if [ "$minfs_path" = "" ]; then
  minfs_path=$build_dir_magenta/tools/minfs
fi

if [ ! -f "$minfs_path" ]; then
  echo "minfs path not found, please build minfs for your host and supply the" \
    "path"
  exit -1
fi

disk_path="${STAGING_DIR}/user_fs"
disk_path_efi="${STAGING_DIR}/efi_fs"

if [ ! -d "$build_dir_fuchsia" ]; then
  echo "Output directory '$build_dir_fuchsia' not found, please make sure you've"\
    "supplied the right build type and architecture OR correct path."
  exit -1
fi

if [ ! -d  "$STAGING_DIR" ]; then
  mkdir "$STAGING_DIR"
else
  rm -rf -- "$STAGING_DIR"/*
fi

# create a suitably large file
echo "Creating system disk image, this may take some time..."
dd if=/dev/zero of="$disk_path" bs="$BLOCK_SIZE" count="$blocks_sys"
"$minfs_path" "$disk_path" mkfs

echo "Creating EFI disk image, this may take some time..."
dd if=/dev/zero of="$disk_path_efi" bs="$BLOCK_SIZE" count="$blocks_efi"
mkfs.vfat -F 32 "$disk_path_efi"

mcpy_loc=$(which mcopy)
mmd_loc=$(which mmd)
lz4_path=$(which lz4)

"${script_dir}"/imager.py --disk_path="$disk_path" --mcp_path="$mcpy_loc" \
  --mmd_path="$mmd_loc" --lz4_path="$lz4_path" --build_dir="$build_dir_fuchsia" \
  --temp_dir="$STAGING_DIR" --minfs_path="$minfs_path" --arch="$arch" \
  --efi_disk="$disk_path_efi" --build_dir_magenta="$build_dir_magenta"
