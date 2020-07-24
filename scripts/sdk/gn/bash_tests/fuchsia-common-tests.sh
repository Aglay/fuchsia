#!/bin/bash
# Copyright 2020 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
#
# Tests for fuchsia-common.sh library script

# shellcheck disable=SC2034
BT_FILE_DEPS=(
  "scripts/sdk/gn/base/bin/fuchsia-common.sh"
  "scripts/sdk/gn/testdata/meta/manifest.json"
)

MOCKED_SSH_BIN="mocked/ssh"
MOCKED_DEVICE_FINDER="scripts/sdk/gn/base/tools/device-finder"
MOCKED_GSUTIL="mocked/gsutil"
MOCKED_CIPD="mocked/cipd"
MOCKED_PGREP="mocked/pgrep"
MOCKED_KILL="mocked/kill"
# shellcheck disable=SC2034
BT_MOCKED_TOOLS=(
   "${MOCKED_SSH_BIN}"
   "${MOCKED_DEVICE_FINDER}"
   "${MOCKED_GSUTIL}"
   "${MOCKED_CIPD}"
   "${MOCKED_PGREP}"
   "${MOCKED_KILL}"
)

BT_SET_UP() {

    cat > "${BT_TEMP_DIR}/${MOCKED_SSH_BIN}.mock_side_effects" <<"EOF"
      echo "$@"
EOF

  # Add mocked system tools to the path.
  export PATH="${BT_TEMP_DIR}/mocked:${PATH}"

  if [ "$(type -t kill)" = "builtin" ]; then
    kill() {
      "${BT_TEMP_DIR}/${MOCKED_KILL}" "$@"
    }
  fi

  # Copy the SDK manifest to the expected location
  mkdir -p  "${BT_TEMP_DIR}/scripts/sdk/gn/base/meta"
  cp "${BT_TEMP_DIR}/scripts/sdk/gn/testdata/meta/manifest.json" "${BT_TEMP_DIR}/scripts/sdk/gn/base/meta/manifest.json"

  # Make "home" directory in the test dir so the paths are stable."
  mkdir -p "${BT_TEMP_DIR}/test-home"
  export HOME="${BT_TEMP_DIR}/test-home"
  FUCHSIA_WORK_DIR="${HOME}/.fuchsia"

  # Source the library file.
  # shellcheck disable=SC1090
  source "${BT_TEMP_DIR}/scripts/sdk/gn/base/bin/fuchsia-common.sh"
}

TEST_data-dir-by-env() {
  # Tests that the data directory can be overridden by using the FUCHSIA_SDK_DATA_DIR varable.
  export FUCHSIA_SDK_DATA_DIR="${BT_TEMP_DIR}/data-dir-by-env"
  mkdir -p "${FUCHSIA_SDK_DATA_DIR}"

    BT_ASSERT_FUNCTION_EXISTS get-fuchsia-sshconfig-file
  FILE_LOCATION="$(get-fuchsia-sshconfig-file)"
  BT_EXPECT_EQ "${FILE_LOCATION}" "${FUCHSIA_SDK_DATA_DIR}/sshconfig"

}

TEST_fx-warn() {
    BT_ASSERT_FUNCTION_EXISTS fx-warn
    fx-warn "This is a test" 2>fx-warn-stderr.txt > fx-warn-stdout.txt
    BT_EXPECT_FILE_CONTAINS "fx-warn-stderr.txt" "WARNING: This is a test"

    [ ! -s fx-warn-stdout.txt ] || BT_EXPECT_GOOD_STATUS $?
}

TEST_fx-error() {
    BT_ASSERT_FUNCTION_EXISTS fx-error
    fx-error "This is a test" 2>fx-error-stderr.txt > fx-error-stdout.txt
    BT_EXPECT_FILE_CONTAINS "fx-error-stderr.txt" "ERROR: This is a test"

    [ ! -s fx-error-stdout.txt ] || BT_EXPECT_GOOD_STATUS $?
}

TEST_ssh-cmd() {
     BT_ASSERT_FUNCTION_EXISTS ssh-cmd
     cat > "${BT_TEMP_DIR}/${MOCKED_SSH_BIN}.mock_side_effects" <<"EOF"
      echo "$@"
EOF
    ssh-cmd remote-host ls -l > /dev/null
    BT_EXPECT_GOOD_STATUS $?
    # shellcheck disable=SC1090
    source "${BT_TEMP_DIR}/${MOCKED_SSH_BIN}.mock_state"
    EXPECTED_SSH_CMD_LINE=("${BT_TEMP_DIR}/${MOCKED_SSH_BIN}" "-F" "${FUCHSIA_WORK_DIR}/sshconfig" "remote-host" "ls" "-l")
    BT_EXPECT_EQ "${EXPECTED_SSH_CMD_LINE[*]}" "${BT_MOCK_ARGS[*]}"
}


TEST_get-device-name() {
     BT_ASSERT_FUNCTION_EXISTS get-device-name
    cat > "${BT_TEMP_DIR}/${MOCKED_DEVICE_FINDER}.mock_side_effects" <<"EOF"
      echo fe80::4607:bff:fe69:b53e%enx44070b69b53f atom-device-name-mocked
EOF
    DEVICE_NAME="$(get-device-name)"
    BT_EXPECT_EQ "${DEVICE_NAME}" "atom-device-name-mocked"
}

TEST_get-device-ip-by-name() {
    BT_ASSERT_FUNCTION_EXISTS get-device-ip-by-name
    MOCK_DEVICE="atom-device-name-mocked"
    get-device-ip-by-name "$(get-fuchsia-sdk-dir)" "${MOCK_DEVICE}"
    # shellcheck disable=SC1090
    source "${BT_TEMP_DIR}/${MOCKED_DEVICE_FINDER}.mock_state"
    EXPECTED_DEVICE_FINDER_CMD_LINE=("${BT_TEMP_DIR}/${MOCKED_DEVICE_FINDER}" "resolve" "-device-limit" "1" "-ipv4=false" "-netboot" "${MOCK_DEVICE}")
    BT_EXPECT_EQ "${EXPECTED_DEVICE_FINDER_CMD_LINE[*]}" "${BT_MOCK_ARGS[*]}"
}

TEST_get-device-ip-by-name-no-args() {
    BT_ASSERT_FUNCTION_EXISTS get-device-ip-by-name
    BT_EXPECT get-device-ip-by-name > /dev/null
    # shellcheck disable=SC1090
    source "${BT_TEMP_DIR}/${MOCKED_DEVICE_FINDER}.mock_state"
    EXPECTED_DEVICE_FINDER_CMD_LINE=("${BT_TEMP_DIR}/${MOCKED_DEVICE_FINDER}" "list" "-netboot" "-device-limit" "1" "-ipv4=false")
    BT_EXPECT_EQ "${EXPECTED_DEVICE_FINDER_CMD_LINE[*]}" "${BT_MOCK_ARGS[*]}"
}

TEST_get-device-ip() {
    BT_ASSERT_FUNCTION_EXISTS get-device-ip
    MOCK_DEVICE="atom-device-name-mocked"
    BT_EXPECT get-device-ip
    # shellcheck disable=SC1090
    source "${BT_TEMP_DIR}/${MOCKED_DEVICE_FINDER}.mock_state"
    EXPECTED_DEVICE_FINDER_CMD_LINE=("${BT_TEMP_DIR}/${MOCKED_DEVICE_FINDER}" "list" "-netboot" "-device-limit" "1" "-ipv4=false")
    BT_EXPECT_EQ "${EXPECTED_DEVICE_FINDER_CMD_LINE[*]}" "${BT_MOCK_ARGS[*]}"
}

TEST_get-host-ip-any() {
     BT_ASSERT_FUNCTION_EXISTS get-host-ip
    cat > "${BT_TEMP_DIR}/${MOCKED_DEVICE_FINDER}.mock_side_effects" <<"EOF"
    if [[ "${1}" == list ]]; then
      echo "fe80::4607:bff:fe69:b53e%enx44070b69b53f atom-device-name-mocked"
    else
      echo "fe80::4600:fff:fefe:b555%enx010101010101"
    fi
EOF

    HOST_IP="$(get-host-ip)"
    # shellcheck disable=SC1090
    source  "${BT_TEMP_DIR}/${MOCKED_DEVICE_FINDER}.mock_state.1"
    expected_cmd_line=( "${BT_TEMP_DIR}/${MOCKED_DEVICE_FINDER}" list -netboot -device-limit 1 -full )
    BT_EXPECT_EQ "${expected_cmd_line[*]}" "${BT_MOCK_ARGS[*]}"

    # shellcheck disable=SC1090
    source  "${BT_TEMP_DIR}/${MOCKED_DEVICE_FINDER}.mock_state.2"
    expected_cmd_line=( "${BT_TEMP_DIR}/${MOCKED_DEVICE_FINDER}" resolve -local "-ipv4=false" atom-device-name-mocked )
    BT_EXPECT_EQ "${expected_cmd_line[*]}" "${BT_MOCK_ARGS[*]}"

    BT_EXPECT_EQ  "${HOST_IP}" "fe80::4600:fff:fefe:b555"
}

TEST_get-host-ip() {
     BT_ASSERT_FUNCTION_EXISTS get-host-ip
    cat > "${BT_TEMP_DIR}/${MOCKED_DEVICE_FINDER}.mock_side_effects" <<"EOF"
      echo "fe80::4600:fff:fefe:b555%enx010101010101"
EOF

    HOST_IP="$(get-host-ip "$(get-fuchsia-sdk-dir)" "atom-device-name-mocked")"
    # shellcheck disable=SC1090
    source  "${BT_TEMP_DIR}/${MOCKED_DEVICE_FINDER}.mock_state"
    expected_cmd_line=( "${BT_TEMP_DIR}/${MOCKED_DEVICE_FINDER}" resolve -local "-ipv4=false" "atom-device-name-mocked" )
    BT_EXPECT_EQ "${expected_cmd_line[*]}" "${BT_MOCK_ARGS[*]}"
    BT_EXPECT_EQ  "${HOST_IP}" "fe80::4600:fff:fefe:b555"
}

TEST_get-sdk-version() {
  BT_ASSERT_FUNCTION_EXISTS get-sdk-version
  SDK_VERSION="$(get-sdk-version)"
  BT_EXPECT_EQ "${SDK_VERSION}" "8890373976687374912"
}

TEST_run-gsutil() {
  BT_ASSERT_FUNCTION_EXISTS run-gsutil
  cat > "${BT_TEMP_DIR}/${MOCKED_GSUTIL}.mock_side_effects" <<EOF
    echo "gs://fuchsia/development/LATEST"
EOF
  RESULT="$(run-gsutil ls gs://fuchsia/development/LATEST)"
  BT_EXPECT_EQ "${RESULT}" "gs://fuchsia/development/LATEST"
}

TEST_run-gsutil-not-found() {
  # override the builtin command which is used to
  # find programs via the PATH environment variable.
  BT_EXPECT_EQ "$(type -t command)"  "builtin"
  command() {
    return 1
  }

  # Capture the return code and any output.
  rc=0
  RESULT="$(run-gsutil ls gs://fuchsia/development 2>&1)"
  rc=$?

  BT_EXPECT_EQ  $rc 1
  BT_EXPECT_EQ "${RESULT}" "ERROR: Cannot find gsutil."
}

TEST_run-cipd() {
  BT_ASSERT_FUNCTION_EXISTS run-cipd
  cat > "${BT_TEMP_DIR}/${MOCKED_CIPD}.mock_side_effects" <<EOF
    echo "fuchsia/"
EOF
  RESULT="$(run-cipd ls)"
  BT_EXPECT_EQ "${RESULT}" "fuchsia/"
}

TEST_run-cipd-not-found() {
  # override the builtin command which is used to
  # find programs via the PATH environment variable.
  BT_EXPECT_EQ "$(type -t command)"  "builtin"
  command() {
    return 1
  }

  # Capture the return code and any output.  
  rc=0
  RESULT="$(run-cipd ls  2>&1)"
  rc=$?
  
  BT_EXPECT_EQ  $rc 1
  BT_EXPECT_EQ "${RESULT}" "ERROR: Cannot find cipd."
}

TEST_get-available-images() {
  BT_ASSERT_FUNCTION_EXISTS get-available-images
  cat > "${BT_TEMP_DIR}/${MOCKED_GSUTIL}.mock_side_effects" <<"EOF"
    if [[ "${2}" == gs://fuchsia* ]]; then
      echo "gs://fuchsia/development/sdk_id/images/image1.tgz"
      echo "gs://fuchsia/development/sdk_id/images/image2.tgz"
      echo "gs://fuchsia/development/sdk_id/images/image3.tgz"
    elif [[ "${2}" == gs://other* ]]; then
      echo "gs://other/development/sdk_id/images/image4.tgz"
      echo "gs://other/development/sdk_id/images/image5.tgz"
      echo "gs://other/development/sdk_id/images/image6.tgz"
    else
      echo "CommandException: One or more URLs matched no objects."
      exit 1
    fi
EOF
  RESULT_LIST=()
  IFS=' ' read -r -a RESULT_LIST <<< "$(get-available-images "sdk_id")"
  BT_EXPECT_EQ "${RESULT_LIST[*]}" "image1 image2 image3"

  IFS=' ' read -r -a RESULT_LIST <<< "$(get-available-images "sdk_id" "other")"
  BT_EXPECT_EQ "${RESULT_LIST[*]}" "image4 image5 image6 image1 image2 image3"

  BT_EXPECT_FAIL get-available-images "sdk_id" "invalid"
}

TEST_kill-running-pm_not_found() {
  cat > "${BT_TEMP_DIR}/${MOCKED_PGREP}.mock_side_effects" <<"EOF"
      echo ""
EOF

  BT_EXPECT kill-running-pm 2> "${BT_TEMP_DIR}/kill-running-pm_output.txt"
  BT_EXPECT_FILE_CONTAINS "${BT_TEMP_DIR}/kill-running-pm_output.txt" "WARNING: existing pm process not found"
}

TEST_kill-running-pm_found() {
  if is-mac; then
    pgrep_result="987654321"
  else
    pgrep_result="987654321 /path/tools/pm"
  fi
  cat > "${BT_TEMP_DIR}/${MOCKED_PGREP}.mock_side_effects" <<EOF
      echo "${pgrep_result}"
EOF

  BT_EXPECT kill-running-pm 2> "${BT_TEMP_DIR}/kill-running-pm_output.txt"
  BT_EXPECT_FILE_CONTAINS "${BT_TEMP_DIR}/kill-running-pm_output.txt" "WARNING: Killing existing pm process"
}

TEST_get-fuchsia-property-names() {
  # Don't copy the property names here, just confirm it is a list and they are valid.
  BT_ASSERT_FUNCTION_EXISTS get-fuchsia-property-names
  BT_ASSERT_FUNCTION_EXISTS is-valid-fuchsia-property

  IFS=" " read -r -a prop_list <<< "$(get-fuchsia-property-names)"

  # There should be between 1 and 10 properties
  BT_ASSERT "(( ${#prop_list[@]} > 1 && ${#prop_list[@]} <= 10  ))"

  for prop in "${prop_list[@]}"; do
    BT_ASSERT is-valid-fuchsia-property "${prop}"
  done
}


TEST_is-valid-fuchsia-property()  {
  BT_ASSERT is-valid-fuchsia-property "device-ip"
  BT_ASSERT_FAIL  is-valid-fuchsia-property "random-value"
}

TEST_set_and_get-fuchsia-property() {
  BT_ASSERT_FUNCTION_EXISTS get-fuchsia-property
  BT_ASSERT_FUNCTION_EXISTS set-fuchsia-property

  BT_EXPECT set-fuchsia-property "device-ip" 100
  value="$(get-fuchsia-property "device-ip")"
  BT_EXPECT_EQ "$value" "100"

  BT_EXPECT set-fuchsia-property "device-ip" ""
  value="$(get-fuchsia-property "device-ip")"
  BT_EXPECT_EQ "$value" ""
}

TEST_migrate_ssh_key() {
  BT_ASSERT_FUNCTION_EXISTS get-fuchsia-auth-keys-file

  # Generate a key in the old location
  legacy_ssh_dir="$(get-fuchsia-sdk-data-dir)/.ssh"
  local legacy_keyfile="${legacy_ssh_dir}/pkey"
  legacy_authfile="${legacy_ssh_dir}/authorized_keys"
  mkdir -p "${legacy_ssh_dir}"
  echo "legacy private key" > "${legacy_keyfile}"
  echo "legacy authorized keys" > "${legacy_authfile}"

  local authfile
  authfile="$(get-fuchsia-auth-keys-file)"
  BT_EXPECT_EQ "${authfile}" "${HOME}/.ssh/fuchsia_authorized_keys"
  BT_ASSERT_FILE_CONTAINS "${authfile}" "legacy authorized keys"

  local keyfile="${HOME}/.ssh/fuchsia_ed25519"
  BT_ASSERT_FILE_CONTAINS "${keyfile}" "legacy private key"
}

BT_RUN_TESTS "$@"
