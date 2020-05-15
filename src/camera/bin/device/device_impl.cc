// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/camera/bin/device/device_impl.h"

#include <lib/async-loop/default.h>
#include <lib/async/cpp/task.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/zx/time.h>
#include <zircon/errors.h>
#include <zircon/types.h>

#include "src/camera/bin/device/messages.h"
#include "src/camera/bin/device/util.h"
#include "src/lib/fsl/handles/object_info.h"

DeviceImpl::DeviceImpl() : loop_(&kAsyncLoopConfigNoAttachToCurrentThread) {}

DeviceImpl::~DeviceImpl() {
  Unbind(controller_);
  async::PostTask(loop_.dispatcher(), [this] { loop_.Quit(); });
  loop_.JoinThreads();
}

fit::result<std::unique_ptr<DeviceImpl>, zx_status_t> DeviceImpl::Create(
    fidl::InterfaceHandle<fuchsia::camera2::hal::Controller> controller,
    fidl::InterfaceHandle<fuchsia::sysmem::Allocator> allocator) {
  auto device = std::make_unique<DeviceImpl>();

  ZX_ASSERT(zx::event::create(0, &device->bad_state_event_) == ZX_OK);

  ZX_ASSERT(device->allocator_.Bind(std::move(allocator), device->loop_.dispatcher()) == ZX_OK);

  constexpr auto kControllerDisconnected = ZX_USER_SIGNAL_0;
  constexpr auto kGetDeviceInfoReturned = ZX_USER_SIGNAL_1;
  constexpr auto kGetConfigsReturned = ZX_USER_SIGNAL_2;
  zx::event event;
  ZX_ASSERT(zx::event::create(0, &event) == ZX_OK);

  // Bind the controller interface and get some initial startup information.

  ZX_ASSERT(device->controller_.Bind(std::move(controller), device->loop_.dispatcher()) == ZX_OK);

  zx_status_t controller_status = ZX_OK;
  device->controller_.set_error_handler([&](zx_status_t status) {
    FX_PLOGS(ERROR, status) << "Controller server disconnected during initialization.";
    controller_status = status;
    ZX_ASSERT(event.signal(0, kControllerDisconnected) == ZX_OK);
  });

  device->controller_->GetDeviceInfo(
      [&, device = device.get()](fuchsia::camera2::DeviceInfo device_info) {
        device->device_info_ = std::move(device_info);
        ZX_ASSERT(event.signal(0, kGetDeviceInfoReturned) == ZX_OK);
      });

  zx_status_t get_next_config_status = ZX_OK;
  fit::function<void(std::unique_ptr<fuchsia::camera2::hal::Config>, zx_status_t)>
      get_next_config_callback =
          [&, device = device.get()](std::unique_ptr<fuchsia::camera2::hal::Config> config,
                                     zx_status_t status) {
            get_next_config_status = status;
            if (status == ZX_OK) {
              auto result = Convert(*config);
              if (result.is_error()) {
                get_next_config_status = result.error();
                FX_PLOGS(ERROR, get_next_config_status);
                return;
              }
              device->configurations_.push_back(result.take_value());
              device->configs_.push_back(std::move(*config));

              // Call again to get remaining configs.
              device->controller_->GetNextConfig(get_next_config_callback.share());
              return;
            }
            if (status == ZX_ERR_STOP) {
              get_next_config_status = ZX_OK;
              device->SetConfiguration(0);
            } else {
              get_next_config_status = ZX_ERR_INTERNAL;
              FX_PLOGS(ERROR, status)
                  << "Controller unexpectedly returned error or null/empty configs list.";
            }

            ZX_ASSERT(event.signal(0, kGetConfigsReturned) == ZX_OK);
          };

  device->controller_->GetNextConfig(get_next_config_callback.share());

  // Start the device thread and begin processing messages.

  ZX_ASSERT(device->loop_.StartThread("Camera Device Thread") == ZX_OK);

  // Wait for either an error, or for all expected callbacks to occur.

  zx_signals_t signaled{};
  ZX_ASSERT(WaitMixed(event, kGetDeviceInfoReturned | kGetConfigsReturned, kControllerDisconnected,
                      zx::time::infinite(), &signaled) == ZX_OK);
  if (signaled & kControllerDisconnected) {
    FX_PLOGS(ERROR, controller_status);
    return fit::error(controller_status);
  }

  // Rebind the controller error handler.

  ZX_ASSERT(async::PostTask(device->loop_.dispatcher(), [device = device.get()]() {
              device->controller_.set_error_handler(
                  fit::bind_member(device, &DeviceImpl::OnControllerDisconnected));
            }) == ZX_OK);

  return fit::ok(std::move(device));
}

fidl::InterfaceRequestHandler<fuchsia::camera3::Device> DeviceImpl::GetHandler() {
  return fit::bind_member(this, &DeviceImpl::OnNewRequest);
}

zx::event DeviceImpl::GetBadStateEvent() {
  zx::event event;
  ZX_ASSERT(bad_state_event_.duplicate(ZX_RIGHTS_BASIC, &event) == ZX_OK);
  return event;
}

void DeviceImpl::OnNewRequest(fidl::InterfaceRequest<fuchsia::camera3::Device> request) {
  PostBind(std::move(request), true);
}

void DeviceImpl::PostBind(fidl::InterfaceRequest<fuchsia::camera3::Device> request,
                          bool exclusive) {
  auto task = [this, request = std::move(request), exclusive]() mutable {
    if (exclusive && !clients_.empty()) {
      request.Close(ZX_ERR_ALREADY_BOUND);
      return;
    }
    auto client = std::make_unique<Client>(*this, client_id_next_, std::move(request));
    clients_.emplace(client_id_next_++, std::move(client));
    if (exclusive) {
      SetConfiguration(0);
    }
  };
  ZX_ASSERT(async::PostTask(loop_.dispatcher(), std::move(task)) == ZX_OK);
}

void DeviceImpl::OnControllerDisconnected(zx_status_t status) {
  FX_PLOGS(ERROR, status) << "Controller disconnected unexpectedly.";
  ZX_ASSERT(bad_state_event_.signal(0, ZX_EVENT_SIGNALED) == ZX_OK);
}

void DeviceImpl::PostRemoveClient(uint64_t id) {
  ZX_ASSERT(async::PostTask(loop_.dispatcher(), [this, id]() { clients_.erase(id); }) == ZX_OK);
}

void DeviceImpl::PostSetConfiguration(uint32_t index) {
  ZX_ASSERT(async::PostTask(loop_.dispatcher(), [this, index]() { SetConfiguration(index); }) ==
            ZX_OK);
}

void DeviceImpl::SetConfiguration(uint32_t index) {
  streams_.clear();
  streams_.resize(configurations_[index].streams.size());
  current_configuration_index_ = index;
  for (auto& client : clients_) {
    client.second->PostConfigurationUpdated(current_configuration_index_);
  }
}

void DeviceImpl::PostConnectToStream(uint32_t index,
                                     fidl::InterfaceRequest<fuchsia::camera3::Stream> request) {
  ZX_ASSERT(
      async::PostTask(loop_.dispatcher(), [this, index, request = std::move(request)]() mutable {
        ConnectToStream(index, std::move(request));
      }) == ZX_OK);
}

void DeviceImpl::ConnectToStream(uint32_t index,
                                 fidl::InterfaceRequest<fuchsia::camera3::Stream> request) {
  if (index > streams_.size()) {
    request.Close(ZX_ERR_INVALID_ARGS);
    return;
  }

  if (streams_[index]) {
    request.Close(ZX_ERR_ALREADY_BOUND);
    return;
  }

  // Once the necessary token is received, post a task to send the request to the controller.
  auto on_stream_requested =
      [this, index](fidl::InterfaceHandle<fuchsia::sysmem::BufferCollectionToken> token,
                    fidl::InterfaceRequest<fuchsia::camera2::Stream> request,
                    fit::function<void(uint32_t)> max_camping_buffers_callback,
                    uint32_t format_index) {
        FX_LOGS(DEBUG) << "New request for legacy stream.";
        ZX_ASSERT(async::PostTask(
                      loop_.dispatcher(),
                      [this, index, token = std::move(token), request = std::move(request),
                       max_camping_buffers_callback = std::move(max_camping_buffers_callback),
                       format_index]() mutable {
                        OnStreamRequested(index, std::move(token), std::move(request),
                                          std::move(max_camping_buffers_callback), format_index);
                      }) == ZX_OK);
      };

  // When the last client disconnects, post a task to the device thread to destroy the stream.
  auto on_no_clients = [this, index]() {
    ZX_ASSERT(async::PostTask(loop_.dispatcher(), [this, index]() { streams_[index] = nullptr; }) ==
              ZX_OK);
  };

  streams_[index] = std::make_unique<StreamImpl>(
      configurations_[current_configuration_index_].streams[index],
      configs_[current_configuration_index_].stream_configs[index], std::move(request),
      std::move(on_stream_requested), std::move(on_no_clients));
}

void DeviceImpl::OnStreamRequested(
    uint32_t index, fidl::InterfaceHandle<fuchsia::sysmem::BufferCollectionToken> token,
    fidl::InterfaceRequest<fuchsia::camera2::Stream> request,
    fit::function<void(uint32_t)> max_camping_buffers_callback, uint32_t format_index) {
  // Negotiate buffers for this stream.
  // TODO(44770): Watch for buffer collection events.
  fuchsia::sysmem::BufferCollectionPtr collection;
  allocator_->BindSharedCollection(std::move(token), collection.NewRequest(loop_.dispatcher()));
  collection->SetConstraints(
      true, configs_[current_configuration_index_].stream_configs[index].constraints);
  collection->WaitForBuffersAllocated(
      [this, index, format_index, request = std::move(request),
       max_camping_buffers_callback = std::move(max_camping_buffers_callback),
       collection = std::move(collection)](
          zx_status_t status, fuchsia::sysmem::BufferCollectionInfo_2 buffers) mutable {
        if (status != ZX_OK) {
          FX_PLOGS(ERROR, status) << "Failed to allocate buffers for stream.";
          request.Close(status);
          return;
        }

        // Inform the stream of the maxmimum number of buffers it may hand out.
        uint32_t max_camping_buffers =
            buffers.buffer_count - configs_[current_configuration_index_]
                                       .stream_configs[index]
                                       .constraints.min_buffer_count_for_camping;
        max_camping_buffers_callback(max_camping_buffers);

        // Assign friendly names to each buffer for debugging and profiling.
        for (uint32_t i = 0; i < buffers.buffer_count; ++i) {
          std::ostringstream oss;
          oss << "camera_c" << current_configuration_index_ << "_s" << index << "_b" << i;
          fsl::MaybeSetObjectName(buffers.buffers[i].vmo.get(), oss.str(), [](std::string s) {
            return s.find("Sysmem") == 0 || s.find("ImagePipe2") == 0;
          });
        }

        // Get the legacy stream using the negotiated buffers.
        controller_->CreateStream(current_configuration_index_, index, format_index,
                                  std::move(buffers), std::move(request));

        collection->Close();
      });
}
