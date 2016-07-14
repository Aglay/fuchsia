#!/bin/bash
# Copyright 2016 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

set -e

readonly SCRIPT_ROOT="$(cd $(dirname ${BASH_SOURCE[0]} ) && pwd)"
readonly FUCHSIA_URL_BASE="https://storage.googleapis.com/fuchsia-build/fuchsia"

case "$(uname -s)" in
  Darwin)
    readonly HOST_PLATFORM="mac"
    ;;
  Linux)
    readonly HOST_PLATFORM="linux64"
    ;;
  *)
    echo "Unknown operating system. Cannot install build tools."
    exit 1
    ;;
esac

function download_tool() {
  local name="${1}"
  local tool_path="${SCRIPT_ROOT}/${HOST_PLATFORM}/${name}"
  local stamp_path="${tool_path}.stamp"
  local requested_hash="$(cat "${tool_path}.sha1")"
  local tool_url="${FUCHSIA_URL_BASE}/${name}/${HOST_PLATFORM=}/${requested_hash}"

  if [[ ! -f "${stamp_path}" ]] || [[ "${requested_hash}" != "$(cat "${stamp_path}")" ]]; then
    echo "Downloading ${name}..."
    rm -f -- "${tool_path}"
    curl --progress-bar -continue-at=- --location --output "${tool_path}" "${tool_url}"
    chmod a+x "${tool_path}"
    echo "${requested_hash}" > "${stamp_path}"
  fi
}

download_tool ninja

# TODO(abarth): gn doesn't follow the normal pattern because we download our
# copy from Chromium's Google Storage bucket.
readonly GN_PATH="${SCRIPT_ROOT}/${HOST_PLATFORM}/gn"
readonly GN_STAMP_PATH="${GN_PATH}.stamp"
readonly GN_HASH="$(cat "${GN_PATH}.sha1")"
readonly GN_BUCKET=chromium-gn
readonly GN_URL="https://storage.googleapis.com/${GN_BUCKET}/${GN_HASH}"

if [[ ! -f "${GN_STAMP_PATH}" ]] || [[ "${GN_HASH}" != "$(cat "${GN_STAMP_PATH}")" ]]; then
  echo "Downloading gn..."
  rm -f -- "${GN_PATH}"
  curl --progress-bar -continue-at=- --location --output "${GN_PATH}" "${GN_URL}"
  chmod a+x "${GN_PATH}"
  echo "${GN_HASH}" > "${GN_STAMP_PATH}"
fi

readonly SDK_PATH="${SCRIPT_ROOT}/sdk"
readonly SDK_STAMP_PATH="${SDK_PATH}.stamp"
readonly SDK_HASH="$(cat "${SDK_PATH}.sha1")"
readonly SDK_TAR_PATH="${SDK_PATH}/sdk.tar.bz2"
readonly SDK_URL="${FUCHSIA_URL_BASE}/sdk/${SDK_HASH}"

if [[ ! -f "${SDK_STAMP_PATH}" ]] || [[ "${SDK_HASH}" != "$(cat "${SDK_STAMP_PATH}")" ]]; then
  echo "Downloading Fuchsia SDK..."
  rm -rf -- "${SDK_PATH}"
  mkdir -- "${SDK_PATH}"
  curl --progress-bar -continue-at=- --location --output "${SDK_TAR_PATH}" "${SDK_URL}"
  (cd -- "${SDK_PATH}" && tar xf sdk.tar.bz2)
  rm -f -- "${SDK_TAR_PATH}"
  echo "${SDK_HASH}" > "${SDK_STAMP_PATH}"
fi

readonly MKBOOTFS_PATH="${SCRIPT_ROOT}/${HOST_PLATFORM}/mkbootfs"
readonly MKBOOTFS_STAMP_PATH="${MKBOOTFS_PATH}.stamp"
readonly MKBOOTFS_SOURCE="${SCRIPT_ROOT}/../magenta/system/tools/mkbootfs.c"
readonly MKBOOTFS_HASH="$(cd ${SCRIPT_ROOT}/../magenta && git rev-parse HEAD)"
if [[ ! -f "${MKBOOTFS_PATH}" || ! -f "${MKBOOTFS_STAMP_PATH}" || "${MKBOOTFS_HASH}" != "$(cat ${MKBOOTFS_STAMP_PATH})" ]]; then
    echo "Building mkbootfs..."
    rm -f "${MKBOOTFS_PATH}"
    gcc "${MKBOOTFS_SOURCE}" -o "${MKBOOTFS_PATH}"
    echo "${MKBOOTFS_HASH}" > "${MKBOOTFS_STAMP_PATH}"
fi
