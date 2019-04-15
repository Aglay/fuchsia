// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ledger/bin/testing/quit_on_error.h"

#include <lib/fit/function.h>
#include <zircon/errors.h>
#include <zircon/status.h>

#include <sstream>

namespace ledger {

namespace internal {

namespace {
template <typename E>
std::string FidlEnumToString(E e) {
  std::stringstream ss;
  ss << fidl::ToUnderlying(e);
  return ss.str();
}
}  // namespace

StatusTranslater::StatusTranslater(Status status)
    : ok_(status == Status::OK), description_(FidlEnumToString(status)) {}

StatusTranslater::StatusTranslater(zx_status_t status)
    : ok_(status == ZX_OK || status == ZX_ERR_PEER_CLOSED),
      description_(zx_status_get_string(status)) {}

StatusTranslater::StatusTranslater(CreateReferenceStatus status)
    : ok_(status == CreateReferenceStatus::OK),
      description_(FidlEnumToString(status)) {}
}  // namespace internal

bool QuitOnError(fit::closure quit_callback, internal::StatusTranslater status,
                 fxl::StringView description) {
  if (status.ok()) {
    return false;
  }
  FXL_LOG(ERROR) << description << " failed with status "
                 << status.description() << ".";
  quit_callback();
  return true;
}

}  // namespace ledger
