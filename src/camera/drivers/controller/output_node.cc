// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "output_node.h"

#include <zircon/errors.h>
#include <zircon/types.h>

#include "src/lib/syslog/cpp/logger.h"
#include "stream_protocol.h"

namespace camera {

constexpr auto TAG = "camera_controller_output_node";

fit::result<OutputNode*, zx_status_t> OutputNode::CreateOutputNode(
    async_dispatcher_t* dispatcher, StreamCreationData* info, ProcessNode* parent_node,
    const InternalConfigNode& internal_output_node) {
  if (dispatcher == nullptr || info == nullptr || parent_node == nullptr) {
    FX_LOGST(ERROR, TAG) << "Invalid input parameters";
    return fit::error(ZX_ERR_INVALID_ARGS);
  }

  auto output_node = std::make_unique<camera::OutputNode>(
      dispatcher, parent_node, info->stream_config->properties.stream_type(),
      internal_output_node.supported_streams);
  if (!output_node) {
    FX_LOGST(ERROR, TAG) << "Failed to create output ProcessNode";
    return fit::error(ZX_ERR_NO_MEMORY);
  }

  auto client_stream = std::make_unique<camera::StreamImpl>(dispatcher, output_node.get());
  if (!client_stream) {
    FX_LOGST(ERROR, TAG) << "Failed to create StreamImpl";
    return fit::error(ZX_ERR_INTERNAL);
  }

  // Set the client stream.
  output_node->set_client_stream(std::move(client_stream));
  auto result = fit::ok(output_node.get());

  // Add child node info.
  ChildNodeInfo child_info;
  child_info.child_node = std::move(output_node);
  child_info.output_frame_rate = internal_output_node.output_frame_rate;
  parent_node->AddChildNodeInfo(std::move(child_info));
  return result;
}

void OutputNode::OnReadyToProcess(uint32_t buffer_index) {
  ZX_ASSERT(client_stream_ != nullptr);
  client_stream_->FrameReady(buffer_index);
}

void OutputNode::OnFrameAvailable(const frame_available_info_t* /*info*/) {
  // This API is not in use for |kOutputStream|.
  ZX_ASSERT(false);
}

void OutputNode::OnReleaseFrame(uint32_t buffer_index) {
  parent_node_->OnReleaseFrame(buffer_index);
}

zx_status_t OutputNode::Attach(zx::channel channel, fit::function<void(void)> disconnect_handler) {
  return client_stream_->Attach(std::move(channel), std::move(disconnect_handler));
}

}  // namespace camera
