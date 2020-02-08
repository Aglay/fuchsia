#!/bin/bash

SCRIPT_SRC_DIR="$(cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd)"
declare -r SCRIPT_SRC_DIR

# Return failure if any test fails.
set -e

"${SCRIPT_SRC_DIR}/script_runner.sh" fuchsia-common-tests.sh
"${SCRIPT_SRC_DIR}/script_runner.sh" fpublish-test.sh
"${SCRIPT_SRC_DIR}/script_runner.sh" fssh-test.sh
