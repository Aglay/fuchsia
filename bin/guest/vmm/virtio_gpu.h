// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_BIN_GUEST_VMM_VIRTIO_GPU_H_
#define GARNET_BIN_GUEST_VMM_VIRTIO_GPU_H_

#include <fuchsia/guest/device/cpp/fidl.h>
#include <fuchsia/sys/cpp/fidl.h>
#include <lib/component/cpp/startup_context.h>
#include <lib/fidl/cpp/binding.h>
#include <lib/svc/cpp/services.h>
#include <virtio/gpu.h>
#include <virtio/virtio_ids.h>

#include "garnet/bin/guest/vmm/virtio_device.h"

static constexpr uint16_t kVirtioGpuNumQueues = 2;

class VirtioGpu
    : public VirtioComponentDevice<VIRTIO_ID_GPU, kVirtioGpuNumQueues,
                                   virtio_gpu_config_t> {
 public:
  explicit VirtioGpu(const PhysMem& phys_mem);

  zx_status_t Start(
      const zx::guest& guest,
      fidl::InterfaceHandle<fuchsia::ui::input::InputListener> input_listener,
      fidl::InterfaceHandle<fuchsia::guest::device::ViewListener> view_listener,
      fuchsia::sys::Launcher* launcher, async_dispatcher_t* dispatcher);

 private:
  enum class State {
    NOT_READY,
    CONFIG_READY,
    READY,
  } state_ = State::NOT_READY;
  component::Services services_;
  fuchsia::sys::ComponentControllerPtr controller_;
  // Use a sync pointer for consistency of virtual machine execution.
  fuchsia::guest::device::VirtioGpuSyncPtr gpu_;
  fuchsia::guest::device::VirtioGpuPtr events_;

  zx_status_t ConfigureQueue(uint16_t queue, uint16_t size, zx_gpaddr_t desc,
                             zx_gpaddr_t avail, zx_gpaddr_t used);
  zx_status_t Ready(uint32_t negotiated_features);

  void OnConfigChanged();
};

#endif  // GARNET_BIN_GUEST_VMM_VIRTIO_GPU_H_
