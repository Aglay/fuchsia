// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "controller-device.h"

#include <lib/fidl/cpp/binding.h>
#include <lib/sync/completion.h>
#include <stdint.h>
#include <zircon/assert.h>
#include <zircon/threads.h>
#include <zircon/types.h>

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/driver.h>

#include "src/lib/syslog/cpp/logger.h"

namespace camera {

constexpr auto kTag = "camera_controller";

namespace {
enum {
  FRAGMENT_ISP,
  FRAGMENT_GDC,
  FRAGMENT_GE2D,
  FRAGMENT_SYSMEM,
  FRAGMENT_BUTTONS,
  FRAGMENT_COUNT,
};
}  // namespace

void ControllerDevice::DdkUnbindNew(ddk::UnbindTxn txn) {
  ShutDown();
  txn.Reply();
}

void ControllerDevice::DdkRelease() { delete this; }

zx_status_t ControllerDevice::DdkMessage(fidl_msg_t* msg, fidl_txn_t* txn) {
  return fuchsia_hardware_camera_Device_dispatch(this, txn, msg, &fidl_ops);
}

zx_status_t ControllerDevice::GetChannel2(zx_handle_t handle) {
  if (handle == ZX_HANDLE_INVALID) {
    return ZX_ERR_INVALID_ARGS;
  }

  if (controller_ != nullptr) {
    zx::channel channel(handle);
    zxlogf(ERROR, "%s: Camera2 Controller already running", __func__);
    return ZX_ERR_INTERNAL;
  }

  zx::channel channel(handle);
  fidl::InterfaceRequest<fuchsia::camera2::hal::Controller> control_interface(std::move(channel));

  fuchsia::sysmem::AllocatorSyncPtr sysmem_allocator;

  auto status = sysmem_.Connect(sysmem_allocator.NewRequest().TakeChannel());
  if (status != ZX_OK) {
    FX_PLOGST(ERROR, kTag, status) << "Could not setup sysmem allocator";
    return status;
  }

  auto shutdown_callback = [this] {
    shutdown_waiter_.set_handler([this](async_dispatcher_t* dispatcher, async::Wait* wait,
                                        zx_status_t status, const zx_packet_signal_t* signal) {
      controller_ = nullptr;
      // Clear the signal.
      shutdown_event_.signal(kPipelineManagerSignalExitDone, 0u);
    });

    shutdown_waiter_.set_object(shutdown_event_.get());
    shutdown_waiter_.set_trigger(kPipelineManagerSignalExitDone);
    shutdown_waiter_.Begin(loop_.dispatcher());

    controller_->Shutdown();
  };

  if (control_interface.is_valid()) {
    controller_ = std::make_unique<ControllerImpl>(
        parent(), std::move(control_interface), loop_.dispatcher(), isp_, gdc_, ge2d_,
        shutdown_callback, std::move(sysmem_allocator), shutdown_event_);
    return ZX_OK;
  }
  return ZX_ERR_INTERNAL;
}

void ControllerDevice::ShutDown() { loop_.Shutdown(); }

zx_status_t ControllerDevice::StartThread() {
  return loop_.StartThread("camera-controller-loop", &loop_thread_);
}

zx_status_t ControllerDevice::RegisterMicButtonNotification() {
  auto status = buttons_.GetChannel(buttons_client_.NewRequest(loop_.dispatcher()).TakeChannel());
  if (status != ZX_OK) {
    return status;
  }

  buttons_client_.set_error_handler([this](zx_status_t status) {
    FX_PLOGST(ERROR, kTag, status) << "Buttons protocol disconnected";
    controller_ = nullptr;
  });

  zx_status_t register_status = ZX_ERR_BAD_STATE;
  sync_completion_t completion;

  buttons_client_->RegisterNotify(
      (1u << static_cast<uint8_t>(fuchsia::buttons::ButtonType::MUTE)),
      [&register_status, &completion](fuchsia::buttons::Buttons_RegisterNotify_Result status) {
        register_status = status.err();
        sync_completion_signal(&completion);
      });

  buttons_client_.events().OnNotify = [this](fuchsia::buttons::ButtonType type, bool pressed) {
    if (controller_) {
      ZX_ASSERT_MSG(type == fuchsia::buttons::ButtonType::MUTE,
                    "Unknown button type event notification");
      if (pressed) {
        controller_->DisableStreaming();
        return;
      }
      controller_->EnableStreaming();
    }
  };

  sync_completion_wait(&completion, ZX_TIME_INFINITE);
  if (register_status != ZX_OK) {
    FX_LOGST(ERROR, kTag) << "Error registering for mic button notification " << register_status;
    return register_status;
  }
  return ZX_OK;
}

// static
zx_status_t ControllerDevice::Setup(zx_device_t* parent, std::unique_ptr<ControllerDevice>* out) {
  ddk::CompositeProtocolClient composite(parent);
  if (!composite.is_valid()) {
    zxlogf(ERROR, "%s: could not get composite protocoln", __func__);
    return ZX_ERR_NOT_SUPPORTED;
  }

  zx_device_t* fragments[FRAGMENT_COUNT];
  size_t actual;
  composite.GetFragments(fragments, FRAGMENT_COUNT, &actual);
  if (actual != FRAGMENT_COUNT) {
    zxlogf(ERROR, "%s: Could not get fragments", __func__);
    return ZX_ERR_NOT_SUPPORTED;
  }

  ddk::GdcProtocolClient gdc(fragments[FRAGMENT_GDC]);
  if (!gdc.is_valid()) {
    zxlogf(ERROR, "%s: ZX_PROTOCOL_GDC not available", __func__);
    return ZX_ERR_NO_RESOURCES;
  }

  ddk::Ge2dProtocolClient ge2d(fragments[FRAGMENT_GE2D]);
  if (!ge2d.is_valid()) {
    zxlogf(ERROR, "%s: ZX_PROTOCOL_GE2D not available", __func__);
    return ZX_ERR_NO_RESOURCES;
  }

  ddk::IspProtocolClient isp(fragments[FRAGMENT_ISP]);
  if (!isp.is_valid()) {
    zxlogf(ERROR, "%s: ZX_PROTOCOL_ISP not available", __func__);
    return ZX_ERR_NO_RESOURCES;
  }

  ddk::SysmemProtocolClient sysmem(fragments[FRAGMENT_SYSMEM]);
  if (!sysmem.is_valid()) {
    zxlogf(ERROR, "%s: ZX_PROTOCOL_SYSMEM not available", __func__);
    return ZX_ERR_NO_RESOURCES;
  }

  ddk::ButtonsProtocolClient buttons(fragments[FRAGMENT_BUTTONS]);
  if (!buttons.is_valid()) {
    zxlogf(ERROR, "%s: ZX_PROTOCOL_BUTTONS not available", __func__);
    return ZX_ERR_NO_RESOURCES;
  }
  zx::event event;
  auto status = zx::event::create(0, &event);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: Could not create shutdown event", __func__);
    return status;
  }

  auto controller = std::make_unique<ControllerDevice>(
      parent, fragments[FRAGMENT_ISP], fragments[FRAGMENT_GDC], fragments[FRAGMENT_GE2D],
      fragments[FRAGMENT_SYSMEM], fragments[FRAGMENT_BUTTONS], std::move(event));

  status = controller->StartThread();
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: Could not start loop thread", __func__);
    return status;
  }

  status = controller->RegisterMicButtonNotification();
  if (status != ZX_OK) {
    FX_PLOGST(ERROR, kTag, status) << "Failed to register for mic button notification";
    return status;
  }

  *out = std::move(controller);
  return ZX_OK;
}

zx_status_t ControllerDeviceBind(void* /*ctx*/, zx_device_t* device) {
  std::unique_ptr<ControllerDevice> controller_device;
  auto status = camera::ControllerDevice::Setup(device, &controller_device);
  if (status != ZX_OK) {
    FX_PLOGST(ERROR, kTag, status) << "Could not setup camera_controller_device";
    return status;
  }

  status = controller_device->DdkAdd("camera-controller-device");
  if (status != ZX_OK) {
    FX_PLOGST(ERROR, kTag, status) << "Could not add camera_controller_device device";
    return status;
  }

  FX_PLOGST(INFO, kTag, status) << "camera_controller_device driver added";

  // controller device intentionally leaked as it is now held by DevMgr.
  __UNUSED auto* dev = controller_device.release();
  return status;
}

static constexpr zx_driver_ops_t driver_ops = []() {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = ControllerDeviceBind;
  return ops;
}();

}  // namespace camera

// clang-format off
ZIRCON_DRIVER_BEGIN(camera_controller, camera::driver_ops, "camera_ctrl", "0.1", 2)
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_COMPOSITE),
    BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_DID, PDEV_DID_CAMERA_CONTROLLER),
ZIRCON_DRIVER_END(camera_controller)
