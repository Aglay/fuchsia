// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/lib/util/filesystem.h"

#include <fcntl.h>
#include <unistd.h>
#include <memory>

#include <lib/fxl/files/file.h>
#include <lib/fxl/files/unique_fd.h>
#include <lib/fxl/logging.h>
#include <lib/fxl/macros.h>
#include <lib/fxl/strings/string_printf.h>
#include <lib/fxl/strings/string_view.h>
#include <lib/zx/time.h>
#include <zircon/device/vfs.h>
#include <zircon/syscalls.h>

namespace modular {

// For polling minfs.
constexpr fxl::StringView kPersistentFileSystem = "/data";
constexpr fxl::StringView kMinFsName = "minfs";
constexpr zx::duration kMaxPollingDelay = zx::sec(10);

void WaitForMinfs() {
  auto delay = zx::msec(10);
  zx::time now = zx::clock::get_monotonic();
  while (zx::clock::get_monotonic() - now < kMaxPollingDelay) {
    fxl::UniqueFD fd(open(kPersistentFileSystem.data(), O_RDONLY));
    if (fd.is_valid()) {
      char buf[sizeof(vfs_query_info_t) + MAX_FS_NAME_LEN + 1];
      auto* info = reinterpret_cast<vfs_query_info_t*>(buf);
      ssize_t len = ioctl_vfs_query_fs(fd.get(), info, sizeof(buf) - 1);
      FXL_DCHECK(len > (ssize_t)sizeof(vfs_query_info_t));
      fxl::StringView fs_name(info->name, len - sizeof(vfs_query_info_t));
      if (fs_name == kMinFsName) {
        return;
      }
    }

    usleep(delay.to_usecs());
    delay = delay * 2;
  }

  FXL_LOG(WARNING) << kPersistentFileSystem
                   << " is not persistent. Did you forget to configure it?";
}

}  // namespace modular
