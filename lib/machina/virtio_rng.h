// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_LIB_MACHINA_VIRTIO_RNG_H_
#define GARNET_LIB_MACHINA_VIRTIO_RNG_H_

#include <fuchsia/guest/device/cpp/fidl.h>
#include <fuchsia/sys/cpp/fidl.h>
#include <virtio/virtio_ids.h>

#include "garnet/lib/machina/virtio_device.h"

namespace machina {

static constexpr uint16_t kVirtioRngNumQueues = 1;

// virtio-rng has no configuration.
struct virtio_rng_config_t {};

class VirtioRng
    : public VirtioComponentDevice<VIRTIO_ID_RNG, kVirtioRngNumQueues,
                                   virtio_rng_config_t> {
 public:
  explicit VirtioRng(const PhysMem& phys_mem);

  zx_status_t Start(const zx::guest& guest, fuchsia::sys::Launcher* launcher,
                    async_dispatcher_t* dispatcher);

 private:
  fuchsia::sys::ComponentControllerPtr controller_;
  // Use a sync pointer for consistency of virtual machine execution.
  fuchsia::guest::device::VirtioRngSyncPtr rng_;

  zx_status_t ConfigureQueue(uint16_t queue, uint16_t size, zx_gpaddr_t desc,
                             zx_gpaddr_t avail, zx_gpaddr_t used);
  zx_status_t Ready(uint32_t negotiated_features);
};

}  // namespace machina

#endif  // GARNET_LIB_MACHINA_VIRTIO_RNG_H_
