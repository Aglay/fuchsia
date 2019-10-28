// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "controller-protocol.h"

#include <fbl/auto_call.h>

#include "fuchsia/camera2/cpp/fidl.h"
#include "src/lib/syslog/cpp/logger.h"

namespace camera {

ControllerImpl::ControllerImpl(fidl::InterfaceRequest<fuchsia::camera2::hal::Controller> control,
                               async_dispatcher_t* dispatcher, ddk::IspProtocolClient& isp,
                               fit::closure on_connection_closed,
                               fuchsia::sysmem::AllocatorSyncPtr sysmem_allocator)
    : binding_(this), camera_pipeline_manager_(dispatcher, isp, std::move(sysmem_allocator)) {
  binding_.set_error_handler([occ = std::move(on_connection_closed)](zx_status_t status) {
    FX_PLOGS(ERROR, status) << "Client disconnected";
    occ();
  });
  binding_.Bind(std::move(control), dispatcher);
}

zx_status_t ControllerImpl::GetInternalConfiguration(uint32_t config_index,
                                                     InternalConfigInfo** internal_config) {
  if (config_index >= internal_configs_.configs_info.size() || internal_config == nullptr) {
    return ZX_ERR_INVALID_ARGS;
  }

  *internal_config = &internal_configs_.configs_info[config_index];
  return ZX_OK;
}

void ControllerImpl::GetConfigs(GetConfigsCallback callback) {
  PopulateConfigurations();
  callback(fidl::Clone(configs_), ZX_OK);
}

InternalConfigNode* ControllerImpl::GetStreamConfigNode(
    InternalConfigInfo* internal_config, fuchsia::camera2::CameraStreamType stream_config_type) {
  // Internal API, assuming the pointer will be valid always.
  for (auto& stream_info : internal_config->streams_info) {
    auto supported_streams = stream_info.supported_streams;
    if (std::find(supported_streams.begin(), supported_streams.end(), stream_config_type) !=
        supported_streams.end()) {
      return &stream_info;
    }
  }
  return nullptr;
}

void ControllerImpl::CreateStream(uint32_t config_index, uint32_t stream_index,
                                  uint32_t image_format_index,
                                  fuchsia::sysmem::BufferCollectionInfo_2 buffer_collection,
                                  fidl::InterfaceRequest<fuchsia::camera2::Stream> stream) {
  zx_status_t status = ZX_OK;
  auto cleanup = fbl::MakeAutoCall([&stream, &status]() { stream.Close(status); });

  if (config_index >= configs_.size()) {
    FX_LOGS(ERROR) << "Invalid config index " << config_index;
    status = ZX_ERR_INVALID_ARGS;
    return;
  }
  const auto& config = configs_[config_index];

  if (stream_index >= config.stream_configs.size()) {
    FX_LOGS(ERROR) << "Invalid stream index " << stream_index;
    status = ZX_ERR_INVALID_ARGS;
    return;
  }
  const auto& stream_config = config.stream_configs[stream_index];

  if (image_format_index >= stream_config.image_formats.size()) {
    FX_LOGS(ERROR) << "Invalid image format index " << image_format_index;
    status = ZX_ERR_INVALID_ARGS;
    return;
  }

  if (buffer_collection.buffer_count == 0) {
    FX_LOGS(ERROR) << "Invalid buffer count " << buffer_collection.buffer_count;
    status = ZX_ERR_INVALID_ARGS;
    return;
  }

  // Get Internal Configuration
  InternalConfigInfo* internal_config;
  status = GetInternalConfiguration(config_index, &internal_config);
  if (status != ZX_OK) {
    FX_LOGS(ERROR) << "Unable to get Internal configuration" << status;
    return;
  }

  auto isp_stream_impl = std::make_unique<camera::IspStreamProtocol>();
  if (!isp_stream_impl) {
    FX_LOGS(ERROR) << "Failed to create IspStreamProtocol";
    status = ZX_ERR_INVALID_ARGS;
    return;
  }
  // Check the Input Stream Type and see if it is already created
  auto stream_config_node =
      GetStreamConfigNode(internal_config, stream_config.properties.stream_type());
  if (stream_config_node == nullptr) {
    FX_LOGS(ERROR) << "Unable to get Internal stream config node";
    return;
  }

  CameraPipelineInfo info;
  info.output_buffers = std::move(buffer_collection);
  info.image_format_index = image_format_index;
  info.node = *stream_config_node;
  info.stream_config = &stream_config;
  // We now have the stream_config_node which needs to be configured
  // Configure the stream pipeline
  status = camera_pipeline_manager_.ConfigureStreamPipeline(&info, stream);
  if (status != ZX_OK) {
    if (status == ZX_ERR_ALREADY_BOUND) {
      stream.Close(ZX_ERR_ALREADY_BOUND);
    }
    FX_PLOGS(ERROR, status) << "Unable to create Stream Pipeline";
    return;
  }

  cleanup.cancel();
}

void ControllerImpl::EnableStreaming() {}

void ControllerImpl::DisableStreaming() {}

void ControllerImpl::GetDeviceInfo(GetDeviceInfoCallback callback) {
  fuchsia::camera2::DeviceInfo camera_device_info;
  camera_device_info.set_vendor_name(kCameraVendorName);
  camera_device_info.set_product_name(kCameraProductName);
  camera_device_info.set_type(fuchsia::camera2::DeviceType::BUILTIN);
  callback(std::move(camera_device_info));
}

}  // namespace camera
