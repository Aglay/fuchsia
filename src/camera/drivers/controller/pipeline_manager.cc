// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "pipeline_manager.h"

#include <zircon/errors.h>
#include <zircon/types.h>

#include "src/lib/syslog/cpp/logger.h"

namespace camera {

constexpr auto TAG = "camera_controller";

bool HasStreamType(const std::vector<fuchsia::camera2::CameraStreamType>& streams,
                   fuchsia::camera2::CameraStreamType type) {
  if (std::find(streams.begin(), streams.end(), type) == streams.end()) {
    return false;
  }
  return true;
}

const InternalConfigNode* PipelineManager::GetNextNodeInPipeline(StreamCreationData* info,
                                                                 const InternalConfigNode& node) {
  for (const auto& child_node : node.child_nodes) {
    for (uint32_t i = 0; i < child_node.supported_streams.size(); i++) {
      if (child_node.supported_streams[i] == info->stream_config->properties.stream_type()) {
        return &child_node;
      }
    }
  }
  return nullptr;
}

bool PipelineManager::IsStreamAlreadyCreated(StreamCreationData* info, ProcessNode* node) {
  auto requested_stream_type = info->stream_config->properties.stream_type();
  for (const auto& stream_type : node->configured_streams()) {
    if (stream_type == requested_stream_type) {
      return true;
    }
  }
  return false;
}

// NOTE: This API currently supports only single consumer node use cases.
fit::result<fuchsia::sysmem::BufferCollectionInfo_2, zx_status_t> PipelineManager::GetBuffers(
    const InternalConfigNode& producer, StreamCreationData* info,
    ProcessNode* producer_graph_node) {
  fuchsia::sysmem::BufferCollectionInfo_2 buffers;
  auto consumer = GetNextNodeInPipeline(info, producer);
  if (!consumer) {
    FX_LOGST(ERROR, TAG) << "Failed to get next node";
    return fit::error(ZX_ERR_INTERNAL);
  }

  // If the consumer is the client, we use the client buffers
  if (consumer->type == kOutputStream) {
    return fit::ok(std::move(info->output_buffers));
  }

  // The controller will  need to allocate memory using sysmem.
  // TODO(braval): Add support for the case of two consumer nodes, which will be needed for the
  // video conferencing config.
  if (producer_graph_node) {
    // The controller already has allocated an output buffer for this producer,
    // so we just need to use that buffer
    auto status = fidl::Clone(producer_graph_node->output_buffer_collection(), &buffers);
    if (status != ZX_OK) {
      FX_LOGST(ERROR, TAG) << "Failed to allocate shared memory";
      return fit::error(status);
    }
    return fit::ok(std::move(buffers));
  }

  std::vector<fuchsia::sysmem::BufferCollectionConstraints> constraints;
  constraints.push_back(producer.output_constraints);
  constraints.push_back(consumer->input_constraints);

  auto status = memory_allocator_.AllocateSharedMemory(constraints, &buffers);
  if (status != ZX_OK) {
    FX_PLOGS(ERROR, status) << "Failed to allocate shared memory";
    return fit::error(status);
  }
  return fit::ok(std::move(buffers));
}

fit::result<std::unique_ptr<ProcessNode>, zx_status_t> PipelineManager::CreateInputNode(
    StreamCreationData* info) {
  uint8_t isp_stream_type;
  if (info->node.input_stream_type == fuchsia::camera2::CameraStreamType::FULL_RESOLUTION) {
    isp_stream_type = STREAM_TYPE_FULL_RESOLUTION;
  } else if (info->node.input_stream_type ==
             fuchsia::camera2::CameraStreamType::DOWNSCALED_RESOLUTION) {
    isp_stream_type = STREAM_TYPE_DOWNSCALED;
  } else {
    return fit::error(ZX_ERR_INVALID_ARGS);
  }

  auto result = GetBuffers(info->node, info, nullptr);
  if (result.is_error()) {
    FX_PLOGST(ERROR, TAG, result.error()) << "Failed to get buffers";
    return fit::error(result.error());
  }
  auto buffers = std::move(result.value());

  // Use a BufferCollectionHelper to manage the conversion
  // between buffer collection representations.
  BufferCollectionHelper buffer_collection_helper(buffers);

  auto image_format = ConvertHlcppImageFormat2toCType(&info->node.image_formats[0]);

  // Create Input Node
  auto processing_node = std::make_unique<camera::ProcessNode>(
      info->node.type, info->node.image_formats, std::move(buffers),
      info->stream_config->properties.stream_type(), info->node.supported_streams);
  if (!processing_node) {
    FX_LOGST(ERROR, TAG) << "Failed to create Input node";
    return fit::error(ZX_ERR_NO_MEMORY);
  }

  // Create stream with ISP
  auto isp_stream_protocol = std::make_unique<camera::IspStreamProtocol>();
  if (!isp_stream_protocol) {
    FX_LOGST(ERROR, TAG) << "Failed to create ISP stream protocol";
    return fit::error(ZX_ERR_INTERNAL);
  }

  auto status = isp_.CreateOutputStream(
      buffer_collection_helper.GetC(), &image_format,
      reinterpret_cast<const frame_rate_t*>(&info->node.output_frame_rate), isp_stream_type,
      processing_node->hw_accelerator_frame_callback(), isp_stream_protocol->protocol());
  if (status != ZX_OK) {
    FX_PLOGST(ERROR, TAG, status) << "Failed to create output stream on ISP";
    return fit::error(ZX_ERR_INTERNAL);
  }

  // Update the input node with the ISP stream protocol
  processing_node->set_isp_stream_protocol(std::move(isp_stream_protocol));
  return fit::ok(std::move(processing_node));
}

fit::result<OutputNode*, zx_status_t> PipelineManager::CreateGraph(
    StreamCreationData* info, const InternalConfigNode& internal_node, ProcessNode* parent_node) {
  fit::result<OutputNode*, zx_status_t> result;
  auto next_node_internal = GetNextNodeInPipeline(info, internal_node);
  if (!next_node_internal) {
    FX_LOGST(ERROR, TAG) << "Failed to get next node";
    return fit::error(ZX_ERR_INTERNAL);
  }

  switch (next_node_internal->type) {
    // Input Node
    case NodeType::kInputStream: {
      FX_LOGST(ERROR, TAG) << "Child node cannot be input node";
      return fit::error(ZX_ERR_INVALID_ARGS);
    }
    // GDC
    case NodeType::kGdc: {
      auto gdc_result = CreateGdcNode(info, parent_node, *next_node_internal);
      if (gdc_result.is_error()) {
        FX_PLOGST(ERROR, TAG, gdc_result.error()) << "Failed to configure GDC Node";
        // TODO(braval): Handle already configured nodes
        return fit::error(gdc_result.error());
      }
      return CreateGraph(info, *next_node_internal, gdc_result.value());
    }
    // GE2D
    case NodeType::kGe2d: {
      return fit::error(ZX_ERR_NOT_SUPPORTED);
    }
    // Output Node
    case NodeType::kOutputStream: {
      result =
          camera::OutputNode::CreateOutputNode(dispatcher_, info, parent_node, *next_node_internal);
      if (result.is_error()) {
        FX_LOGST(ERROR, TAG) << "Failed to configure Output Node";
        // TODO(braval): Handle already configured nodes
        return result;
      }
      break;
    }
    default: {
      return fit::error(ZX_ERR_NOT_SUPPORTED);
    }
  }
  return result;
}

fit::result<std::unique_ptr<ProcessNode>, zx_status_t>
PipelineManager::ConfigureStreamPipelineHelper(
    StreamCreationData* info, fidl::InterfaceRequest<fuchsia::camera2::Stream>& stream) {
  // Configure Input node
  auto input_result = CreateInputNode(info);
  if (input_result.is_error()) {
    FX_PLOGST(ERROR, TAG, input_result.error()) << "Failed to ConfigureInputNode";
    return input_result;
  }

  auto input_processing_node = std::move(input_result.value());
  ProcessNode* input_node = input_processing_node.get();

  auto output_node_result = CreateGraph(info, info->node, input_node);
  if (output_node_result.is_error()) {
    FX_PLOGST(ERROR, TAG, output_node_result.error()) << "Failed to CreateGraph";
    return fit::error(output_node_result.error());
  }

  auto output_node = output_node_result.value();
  auto status =
      output_node->Attach(stream.TakeChannel(), [this, info]() { OnClientStreamDisconnect(info); });
  if (status != ZX_OK) {
    FX_PLOGST(ERROR, TAG, status) << "Failed to bind output stream";
    return fit::error(status);
  }
  return fit::ok(std::move(input_processing_node));
}

fit::result<std::pair<InternalConfigNode, ProcessNode*>, zx_status_t>
PipelineManager::FindNodeToAttachNewStream(StreamCreationData* info,
                                           const InternalConfigNode& current_internal_node,
                                           ProcessNode* node) {
  auto requested_stream_type = info->stream_config->properties.stream_type();

  // Validate if this node supports the requested stream type
  // to be safe.
  if (!HasStreamType(node->supported_streams(), requested_stream_type)) {
    return fit::error(ZX_ERR_INVALID_ARGS);
  }

  // Traverse the |node| to find a node which supports this stream
  // but none of its children support this stream.
  for (auto& child_node_info : node->child_nodes_info()) {
    if (HasStreamType(child_node_info.child_node->supported_streams(), requested_stream_type)) {
      // If we find a child node which supports the requested stream type,
      // we move on to that child node.
      auto next_internal_node = GetNextNodeInPipeline(info, current_internal_node);
      if (!next_internal_node) {
        FX_LOGS(ERROR) << "Failed to get next node for requested stream";
        return fit::error(ZX_ERR_INTERNAL);
      }
      return FindNodeToAttachNewStream(info, *next_internal_node, child_node_info.child_node.get());
    }
    // This is the node we need to attach the new stream pipeline to
    return fit::ok(std::make_pair(current_internal_node, node));
  }

  // Should not reach here
  FX_LOGS(ERROR) << "Failed FindNodeToAttachNewStream";
  return fit::error(ZX_ERR_INTERNAL);
}

zx_status_t PipelineManager::AppendToExistingGraph(
    StreamCreationData* info, ProcessNode* graph_head,
    fidl::InterfaceRequest<fuchsia::camera2::Stream>& stream) {
  auto result = FindNodeToAttachNewStream(info, info->node, graph_head);
  if (result.is_error()) {
    FX_PLOGS(ERROR, result.error()) << "Failed FindNodeToAttachNewStream";
    return result.error();
  }

  // If the next node is an output node, we currently do not support
  // this. Currently we expect that the clients would request for streams in a fixed order.
  // TODO(42241): Remove this check when 42241 is fixed.
  auto next_node_internal = GetNextNodeInPipeline(info, result.value().first);
  if (!next_node_internal) {
    FX_PLOGS(ERROR, ZX_ERR_INTERNAL) << "Failed to get next node";
    return ZX_ERR_INTERNAL;
  }

  if (next_node_internal->type == NodeType::kOutputStream) {
    FX_PLOGS(ERROR, ZX_ERR_NOT_SUPPORTED)
        << "Cannot create this stream due to unexpected ordering of stream create requests";
    return ZX_ERR_NOT_SUPPORTED;
  }

  auto node_to_be_appended = result.value().second;
  auto output_node_result = CreateGraph(info, result.value().first, node_to_be_appended);
  if (output_node_result.is_error()) {
    FX_PLOGS(ERROR, output_node_result.error()) << "Failed to CreateGraph";
    return output_node_result.error();
  }

  auto output_node = output_node_result.value();
  auto status = output_node->Attach(stream.TakeChannel(), [this, info]() {
    FX_LOGS(INFO) << "Stream client disconnected";
    OnClientStreamDisconnect(info);
  });
  if (status != ZX_OK) {
    FX_PLOGS(ERROR, status) << "Failed to bind output stream";
    return status;
  }

  // Push this new requested stream to all pre-existing nodes |configured_streams| vector
  auto requested_stream_type = info->stream_config->properties.stream_type();
  auto current_node = node_to_be_appended;
  while (current_node) {
    current_node->configured_streams().push_back(requested_stream_type);
    current_node = current_node->parent_node();
  }

  return status;
}

zx_status_t PipelineManager::ConfigureStreamPipeline(
    StreamCreationData* info, fidl::InterfaceRequest<fuchsia::camera2::Stream>& stream) {
  // Input Validations
  if (info == nullptr || info->stream_config == nullptr) {
    return ZX_ERR_INVALID_ARGS;
  }
  // Here at top level we check what type of input stream do we have to deal with
  switch (info->node.input_stream_type) {
    case fuchsia::camera2::CameraStreamType::FULL_RESOLUTION: {
      if (full_resolution_stream_) {
        // If the same stream is requested again, we return failure.
        if (IsStreamAlreadyCreated(info, full_resolution_stream_.get())) {
          FX_PLOGS(ERROR, ZX_ERR_ALREADY_BOUND) << "Stream already bound";
          return ZX_ERR_ALREADY_BOUND;
        }
        // We will now append the requested stream to the existing graph.
        auto result = AppendToExistingGraph(info, full_resolution_stream_.get(), stream);
        if (result != ZX_OK) {
          FX_PLOGST(ERROR, TAG, result) << "AppendToExistingGraph failed";
          return result;
        }
        return result;
      }

      auto result = ConfigureStreamPipelineHelper(info, stream);
      if (result.is_error()) {
        return result.error();
      }
      full_resolution_stream_ = std::move(result.value());
      break;
    }

    case fuchsia::camera2::CameraStreamType::DOWNSCALED_RESOLUTION: {
      if (downscaled_resolution_stream_) {
        // If the same stream is requested again, we return failure.
        if (IsStreamAlreadyCreated(info, downscaled_resolution_stream_.get())) {
          FX_PLOGST(ERROR, TAG, ZX_ERR_ALREADY_BOUND) << "Stream already bound";
          return ZX_ERR_ALREADY_BOUND;
        }
        // We will now append the requested stream to the existing graph.
        auto result = AppendToExistingGraph(info, downscaled_resolution_stream_.get(), stream);
        if (result != ZX_OK) {
          FX_PLOGST(ERROR, TAG, result) << "AppendToExistingGraph failed";
          return result;
        }
        return result;
      }

      auto result = ConfigureStreamPipelineHelper(info, stream);
      if (result.is_error()) {
        return result.error();
      }
      downscaled_resolution_stream_ = std::move(result.value());
      break;
    }
    default: {
      FX_LOGST(ERROR, TAG) << "Invalid input stream type";
      return ZX_ERR_INVALID_ARGS;
    }
  }
  return ZX_OK;
}

void PipelineManager::OnClientStreamDisconnect(StreamCreationData* info) {
  ZX_ASSERT(info != nullptr);
  // TODO(braval): When we add support N > 1 substreams of FR and DS
  // being present at the same time, we need to ensure to only
  // bring down the relevant part of the graph and not the entire
  // graph.
  switch (info->node.input_stream_type) {
    case fuchsia::camera2::CameraStreamType::FULL_RESOLUTION: {
      full_resolution_stream_ = nullptr;
      break;
    }
    case fuchsia::camera2::CameraStreamType::DOWNSCALED_RESOLUTION: {
      downscaled_resolution_stream_ = nullptr;
      break;
    }
    default: {
      ZX_ASSERT_MSG(false, "Invalid input stream type\n");
    }
  }
}

}  // namespace camera
