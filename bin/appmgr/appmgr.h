// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_BIN_APPMGR_APPMGR_H_
#define GARNET_BIN_APPMGR_APPMGR_H_

#include <fs/pseudo-dir.h>
#include <fs/service.h>
#include <fs/synchronous-vfs.h>

#include "garnet/bin/appmgr/realm.h"
#include "garnet/bin/appmgr/root_loader.h"
#include "lib/fxl/macros.h"

namespace fuchsia {
namespace sys {

class Appmgr {
 public:
  Appmgr(async_t* async, zx_handle_t pa_directory_request);
  ~Appmgr();

 private:
  fs::SynchronousVfs loader_vfs_;
  RootLoader root_loader_;
  fbl::RefPtr<fs::PseudoDir> loader_dir_;

  std::unique_ptr<Realm> root_realm_;
  fs::SynchronousVfs publish_vfs_;
  fbl::RefPtr<fs::PseudoDir> publish_dir_;

  ComponentControllerPtr sysmgr_;

  FXL_DISALLOW_COPY_AND_ASSIGN(Appmgr);
};

}  // namespace sys
}  // namespace fuchsia

#endif  // GARNET_BIN_APPMGR_APPMGR_H_
