// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-loop/cpp/loop.h>
#include <lib/fit/defer.h>
#include <trace-provider/provider.h>

#include "garnet/bin/guest/vmm/device/device_base.h"
#include "garnet/bin/guest/vmm/device/stream_base.h"

class RngStream : public StreamBase {
 public:
  void Notify() {
    for (; queue_.NextChain(&chain_); chain_.Return()) {
      while (chain_.NextDescriptor(&desc_)) {
        FXL_CHECK(desc_.writable) << "Descriptor is not writable";
        zx_cprng_draw(desc_.addr, desc_.len);
        *chain_.Used() += desc_.len;
      }
    }
  }
};

// Implementation of a virtio-rng device.
class VirtioRngImpl : public DeviceBase<VirtioRngImpl>,
                      public fuchsia::guest::device::VirtioRng {
 public:
  VirtioRngImpl(component::StartupContext* context) : DeviceBase(context) {}

  // |fuchsia::guest::device::VirtioDevice|
  void NotifyQueue(uint16_t queue) override {
    FXL_CHECK(queue == 0) << "Queue index " << queue << " out of range";
    queue_.Notify();
  }

 private:
  // |fuchsia::guest::device::VirtioRng|
  void Start(fuchsia::guest::device::StartInfo start_info,
             StartCallback callback) override {
    auto deferred = fit::defer(std::move(callback));
    PrepStart(std::move(start_info));
    queue_.Init(phys_mem_, fit::bind_member<zx_status_t, DeviceBase>(
                               this, &VirtioRngImpl::Interrupt));
  }

  // |fuchsia::guest::device::VirtioDevice|
  void ConfigureQueue(uint16_t queue, uint16_t size, zx_gpaddr_t desc,
                      zx_gpaddr_t avail, zx_gpaddr_t used,
                      ConfigureQueueCallback callback) override {
    auto deferred = fit::defer(std::move(callback));
    FXL_CHECK(queue == 0) << "Queue index " << queue << " out of range";
    queue_.Configure(size, desc, avail, used);
  }

  // |fuchsia::guest::device::VirtioDevice|
  void Ready(uint32_t negotiated_features, ReadyCallback callback) override {
    callback();
  }

  RngStream queue_;
};

int main(int argc, char** argv) {
  async::Loop loop(&kAsyncLoopConfigAttachToThread);
  trace::TraceProvider trace_provider(loop.dispatcher());
  std::unique_ptr<component::StartupContext> context =
      component::StartupContext::CreateFromStartupInfo();

  VirtioRngImpl virtio_rng(context.get());
  return loop.Run();
}
