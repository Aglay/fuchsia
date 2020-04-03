#!/bin/sh
# Copyright 2020 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# Copies the debug symbol file in the |build-id-dir| directory with the build
# id of the given |binary| to |output|.  Requires a |build-id-script| to read
# the build id of |binary|.  The goal is to provide the debug symbols with a
# filename statically known to the build system, so that it can be depended on
# explicitly.

set -e

while [ $# != 0 ]; do
  case "$1" in
    --output)
      OUTPUT="$2"
      shift
      ;;
    --build-id-script)
      BUILD_ID_SCRIPT="$2"
      shift
      ;;
    --build-id-dir)
      BUILD_ID_DIR="$2"
      shift
      ;;
    --binary)
      BINARY="$2"
      shift
      ;;
    *)
      break
      ;;
  esac
  shift
done

BUILD_ID=$("${BUILD_ID_SCRIPT}" --build-id "${BINARY}")

BUILD_ID_DIR_1=$(printf "${BUILD_ID}" | head -c 2)
BUILD_ID_DIR_2=$(printf "${BUILD_ID}" | tail -c +3)

/bin/cp "${BUILD_ID_DIR}"/"${BUILD_ID_DIR_1}"/"${BUILD_ID_DIR_2}".debug "${OUTPUT}"
