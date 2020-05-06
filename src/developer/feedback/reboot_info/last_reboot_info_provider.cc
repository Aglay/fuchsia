// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/feedback/reboot_info/last_reboot_info_provider.h"

#include "src/developer/feedback/reboot_info/reboot_reason.h"
#include "src/lib/syslog/cpp/logger.h"

namespace feedback {

LastRebootInfoProvider::LastRebootInfoProvider(const RebootLog& reboot_log) {
  if (reboot_log.HasUptime()) {
    last_reboot_.set_uptime(reboot_log.Uptime().to_msecs());
  }

  last_reboot_.set_graceful(IsGraceful(reboot_log.RebootReason()));

  const auto fidl_reboot_reason = ToFidlRebootReason(reboot_log.RebootReason());
  if (fidl_reboot_reason.has_value()) {
    last_reboot_.set_reason(fidl_reboot_reason.value());
  }
}

void LastRebootInfoProvider::Get(GetCallback callback) {
  fuchsia::feedback::LastReboot last_reboot;

  if (const auto status = last_reboot_.Clone(&last_reboot); status != ZX_OK) {
    FX_PLOGS(ERROR, status) << "Error cloning |last_reboot_|";
  }

  callback(std::move(last_reboot));
}

}  // namespace feedback
