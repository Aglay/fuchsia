// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/feedback/feedback_data/annotations/board_name_provider.h"

#include <fcntl.h>
#include <fuchsia/sysinfo/cpp/fidl.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/fdio.h>
#include <lib/fidl/cpp/string.h>
#include <lib/fidl/cpp/synchronous_interface_ptr.h>
#include <lib/syslog/cpp/macros.h>
#include <zircon/errors.h>
#include <zircon/status.h>
#include <zircon/syscalls.h>
#include <zircon/types.h>

#include "src/developer/feedback/utils/errors.h"

namespace feedback {

AnnotationOr GetBoardName() {
  fuchsia::sysinfo::SysInfoSyncPtr sysinfo;

  if (const zx_status_t status = fdio_service_connect("/svc/fuchsia.sysinfo.SysInfo",
                                                      sysinfo.NewRequest().TakeChannel().release());
      status != ZX_OK) {
    FX_PLOGS(ERROR, status) << "Error connecting to sysinfo";
    return AnnotationOr(Error::kConnectionError);
  }

  ::fidl::StringPtr out_board_name;
  zx_status_t out_status;
  if (const zx_status_t status = sysinfo->GetBoardName(&out_status, &out_board_name);
      status != ZX_OK) {
    FX_PLOGS(ERROR, status) << "Failed to get device board name";
    return AnnotationOr(Error::kConnectionError);
  }
  if (out_status != ZX_OK) {
    FX_PLOGS(ERROR, out_status) << "Failed to get device board name";
    return AnnotationOr(Error::kBadValue);
  }
  if (!out_board_name) {
    FX_PLOGS(ERROR, out_status) << "Failed to get device board name";
    return AnnotationOr(Error::kMissingValue);
  }

  return AnnotationOr(out_board_name.value());
}

}  // namespace feedback
