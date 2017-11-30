#!/bin/bash
# Copyright 2017 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# TODO(INTK-60): this script should be temporary, as futility should later be
# cipd distributed.

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"/devshell/lib/vars.sh
fx-config-read
source "${FUCHSIA_DIR}/buildtools/vars.sh"

export CMAKE_PROGRAM="${FUCHSIA_DIR}/buildtools/${BUILDTOOLS_PLATFORM}/cmake/bin/cmake"
export NINJA_PROGRAM="${FUCHSIA_DIR}/buildtools/ninja"

"${FUCHSIA_DIR}/third_party/vboot_reference/scripts/build-futility.sh"
