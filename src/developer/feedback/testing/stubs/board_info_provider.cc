// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/feedback/testing/stubs/board_info_provider.h"

#include "src/lib/syslog/cpp/logger.h"

namespace feedback {
namespace stubs {

using fuchsia::hwinfo::Board;

void BoardInfoProvider::GetInfo(GetInfoCallback callback) {
  FX_CHECK(!has_been_called_) << "GetInfo() can only be called once";
  has_been_called_ = true;
  callback(std::move(info_));
}

}  // namespace stubs
}  // namespace feedback
