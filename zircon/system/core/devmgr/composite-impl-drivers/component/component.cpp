// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/binding.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddktl/device.h>
#include <lib/zx/channel.h>
#include <memory>

namespace {

class Component;
using ComponentBase = ddk::Device<Component, ddk::Rxrpcable, ddk::Unbindable>;

class Component : public ComponentBase {
public:
    explicit Component(zx_device_t* parent);

    static zx_status_t Bind(void* ctx, zx_device_t* parent);

    zx_status_t DdkRxrpc(zx_handle_t channel);
    void DdkUnbind();
    void DdkRelease();
};

Component::Component(zx_device_t* parent) : ComponentBase(parent) {}

zx_status_t Component::Bind(void* ctx, zx_device_t* parent) {
    auto dev = std::make_unique<Component>(parent);
    // The thing before the comma will become the process name, if a new process
    // is created
    const char* proxy_args = "composite-device,";
    auto status = dev->DdkAdd("component", DEVICE_ADD_NON_BINDABLE | DEVICE_ADD_MUST_ISOLATE,
                              nullptr /* props */, 0 /* prop count */, 0 /* proto id */,
                              proxy_args);
    if (status == ZX_OK) {
        // devmgr owns the memory now
        __UNUSED auto ptr = dev.release();
    }
    return status;
}

zx_status_t Component::DdkRxrpc(zx_handle_t raw_channel) {
    zx::unowned_channel channel(raw_channel);
    if (!channel->is_valid()) {
        // This driver is stateless, so we don't need to reset anything here
        return ZX_OK;
    }
    // TODO(teisenbe): Wire up a proxy half in the right process
    return ZX_ERR_NOT_SUPPORTED;
}

void Component::DdkUnbind() {
    DdkRemove();
}

void Component::DdkRelease() {
    delete this;
}

const zx_driver_ops_t component_driver_ops = []() {
    zx_driver_ops_t ops = {};
    ops.version = DRIVER_OPS_VERSION;
    ops.bind = Component::Bind;
    return ops;
}();

} // namespace

ZIRCON_DRIVER_BEGIN(component, component_driver_ops, "zircon", "0.1", 1)
BI_MATCH() // This driver is excluded from the normal matching process, so this is fine.
ZIRCON_DRIVER_END(component)
