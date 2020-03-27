// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_FEEDBACK_FEEDBACK_AGENT_ATTACHMENTS_KERNEL_LOG_PTR_H_
#define SRC_DEVELOPER_FEEDBACK_FEEDBACK_AGENT_ATTACHMENTS_KERNEL_LOG_PTR_H_

#include <fuchsia/boot/cpp/fidl.h>
#include <lib/async/cpp/task.h>
#include <lib/async/dispatcher.h>
#include <lib/fit/promise.h>
#include <lib/sys/cpp/service_directory.h>
#include <lib/zx/debuglog.h>
#include <lib/zx/time.h>

#include "src/developer/feedback/feedback_agent/attachments/aliases.h"
#include "src/developer/feedback/utils/bridge.h"
#include "src/developer/feedback/utils/cobalt.h"
#include "src/lib/fxl/macros.h"

namespace feedback {

// Retrieves the kernel log. fuchsia.boot.ReadOnlyLog is expected to be in
// |services|.
fit::promise<AttachmentValue> CollectKernelLog(async_dispatcher_t* dispatcher,
                                               std::shared_ptr<sys::ServiceDirectory> services,
                                               zx::duration timeout, Cobalt* cobalt);

// Wraps around fuchsia::boot::ReadOnlyLogPtr to handle establishing the
// connection, losing the connection, waiting for the callback, enforcing a
// timeout, etc.
//
// GetLog() is expected to be called only once.
class BootLog {
 public:
  BootLog(async_dispatcher_t* dispatcher, std::shared_ptr<sys::ServiceDirectory> services,
          Cobalt* cobalt);

  fit::promise<AttachmentValue> GetLog(zx::duration timeout);

 private:
  const std::shared_ptr<sys::ServiceDirectory> services_;
  Cobalt* cobalt_;

  // Enforces the one-shot nature of GetLog().
  bool has_called_get_log_ = false;

  fuchsia::boot::ReadOnlyLogPtr log_ptr_;

  Bridge<AttachmentValue> bridge_;

  FXL_DISALLOW_COPY_AND_ASSIGN(BootLog);
};

}  // namespace feedback

#endif  // SRC_DEVELOPER_FEEDBACK_FEEDBACK_AGENT_ATTACHMENTS_KERNEL_LOG_PTR_H_
