// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/guest/vmm/device/virtio_magma.h"

#include <fcntl.h>
#include <unistd.h>

#include <lib/fxl/logging.h>
#include <trace/event.h>

#include "garnet/bin/guest/vmm/device/virtio_queue.h"

zx_status_t VirtioMagma::Init(std::string device_path) {
  device_path_ = device_path;
  device_fd_ = fbl::unique_fd(open(device_path_.c_str(), O_RDONLY));
  if (!device_fd_.is_valid()) {
    FXL_LOG(ERROR) << "Failed do open device at " << device_path_ << ": "
                   << strerror(errno);
    return ZX_ERR_INTERNAL;
  }

  return ZX_OK;
}

VirtioMagma::~VirtioMagma() {
  // TODO: flush and close all host magma connections
}

#define COMMAND_CASE(cmd_suffix, struct_prefix, method_name, used)            \
  case VIRTIO_MAGMA_CMD_##cmd_suffix: {                                       \
    auto request = reinterpret_cast<const virtio_magma_##struct_prefix##_t*>( \
        request_desc.addr);                                                   \
    auto response = reinterpret_cast<virtio_magma_##struct_prefix##_resp_t*>( \
        response_desc.addr);                                                  \
    if (response_desc.len >= sizeof(*response)) {                             \
      method_name(request, response);                                         \
      (used) = sizeof(*response);                                             \
    } else {                                                                  \
      FXL_LOG(ERROR) << "MAGMA command "                                      \
                     << "(" << command_type << ") "                           \
                     << "response descriptor too small";                      \
    }                                                                         \
  } break

void VirtioMagma::HandleCommand(VirtioChain* chain) {
  TRACE_DURATION("machina", "VirtioMagma::HandleCommand");
  VirtioDescriptor request_desc;
  if (!chain->NextDescriptor(&request_desc)) {
    FXL_LOG(ERROR) << "Failed to read request descriptor";
    return;
  }
  const auto request_header =
      reinterpret_cast<virtio_magma_ctrl_hdr_t*>(request_desc.addr);
  const uint32_t command_type = request_header->type;

  if (chain->HasDescriptor()) {
    VirtioDescriptor response_desc{};
    if (!chain->NextDescriptor(&response_desc)) {
      FXL_LOG(ERROR) << "Failed to read descriptor";
      return;
    }
    if (response_desc.writable) {
      switch (command_type) {
        COMMAND_CASE(QUERY, query, Query, *chain->Used());
        COMMAND_CASE(CREATE_CONNECTION, create_connection, CreateConnection,
                     *chain->Used());
        COMMAND_CASE(RELEASE_CONNECTION, release_connection, ReleaseConnection,
                     *chain->Used());
        default: {
          FXL_LOG(ERROR) << "Unsupported MAGMA command "
                         << "(" << command_type << ")";
          auto response =
              reinterpret_cast<virtio_magma_ctrl_hdr_t*>(response_desc.addr);
          response->type = VIRTIO_MAGMA_RESP_ERR_INVALID_COMMAND;
          *chain->Used() = sizeof(*response);
        } break;
      }
    } else {
      FXL_LOG(ERROR) << "MAGMA command "
                     << "(" << command_type << ") "
                     << "response descriptor is not writable";
    }
  } else {
    FXL_LOG(ERROR) << "MAGMA command "
                   << "(" << command_type << ") "
                   << "does not contain a response descriptor";
  }
  chain->Return();
}

void VirtioMagma::OnCommandAvailable() {
  TRACE_DURATION("machina", "VirtioMagma::OnCommandAvailable");
  VirtioChain chain;
  while (out_queue_->NextChain(&chain)) {
    HandleCommand(&chain);
  }
}

void VirtioMagma::OnQueueReady() {}

void VirtioMagma::Query(const virtio_magma_query_t* request,
                        virtio_magma_query_resp_t* response) {
  TRACE_DURATION("machina", "VirtioMagma::Query");
  FXL_DCHECK(request->hdr.type == VIRTIO_MAGMA_CMD_QUERY);
  if (request->field_id == MAGMA_QUERY_DEVICE_ID ||
      request->field_id >= MAGMA_QUERY_VENDOR_PARAM_0) {
    uint64_t field_value_out = 0;
    magma_status_t status =
        magma_query(device_fd_.get(), request->field_id, &field_value_out);
    response->hdr.type = VIRTIO_MAGMA_RESP_QUERY;
    response->field_value_out = field_value_out;
    response->status_return = status;
  } else {
    response->hdr.type = VIRTIO_MAGMA_RESP_ERR_INVALID_ARGUMENT;
  }
}

void VirtioMagma::CreateConnection(
    const virtio_magma_create_connection_t* request,
    virtio_magma_create_connection_resp_t* response) {
  TRACE_DURATION("machina", "VirtioMagma::CreateConnection");
  FXL_DCHECK(request->hdr.type == VIRTIO_MAGMA_CMD_CREATE_CONNECTION);
  magma_connection_t connection;
  magma_status_t status =
      magma_create_connection(device_fd_.get(), &connection);
  if (status != MAGMA_STATUS_OK) {
    response->connection_return = -1;
    response->hdr.type = VIRTIO_MAGMA_RESP_ERR_HOST_DISCONNECTED;
    return;
  }
  FXL_LOG(INFO) << "magma connection created (" << next_connection_id_ << ")";
  connections_.insert({next_connection_id_, connection});
  response->connection_return = next_connection_id_++;
  response->hdr.type = VIRTIO_MAGMA_RESP_CREATE_CONNECTION;
}

void VirtioMagma::ReleaseConnection(
    const virtio_magma_release_connection_t* request,
    virtio_magma_release_connection_resp_t* response) {
  TRACE_DURATION("machina", "VirtioMagma::ReleaseConnection");
  FXL_DCHECK(request->hdr.type == VIRTIO_MAGMA_CMD_RELEASE_CONNECTION);
  auto connection = connections_.find(request->connection);
  if (connection == connections_.end()) {
    FXL_LOG(ERROR) << "invalid connection (" << request->connection << ")";
    return;
  }
  FXL_LOG(INFO) << "magma connection released (" << request->connection << ")";
  connections_.erase(connection);
}
