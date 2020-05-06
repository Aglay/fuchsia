// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_FEEDBACK_REBOOT_INFO_LAST_REBOOT_INFO_PROVIDER_H_
#define SRC_DEVELOPER_FEEDBACK_REBOOT_INFO_LAST_REBOOT_INFO_PROVIDER_H_

#include <fuchsia/feedback/cpp/fidl.h>

#include "src/developer/feedback/reboot_info/reboot_log.h"

namespace feedback {

class LastRebootInfoProvider : public fuchsia::feedback::LastRebootInfoProvider {
 public:
  LastRebootInfoProvider(const RebootLog& reboot_log);

  // |fuchsia::feedback::LastRebootInfoProvider|
  void Get(GetCallback callback) override;

 private:
  fuchsia::feedback::LastReboot last_reboot_;
};

}  // namespace feedback

#endif  // SRC_DEVELOPER_FEEDBACK_REBOOT_INFO_LAST_REBOOT_INFO_PROVIDER_H_
