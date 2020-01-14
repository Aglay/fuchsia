// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "pipeline_manager.h"

#include <zircon/errors.h>
#include <zircon/types.h>

#include "graph_utils.h"
#include "src/lib/syslog/cpp/logger.h"

namespace camera {

constexpr auto kTag = "camera_controller";

fit::result<OutputNode*, zx_status_t> PipelineManager::CreateGraph(
    StreamCreationData* info, const InternalConfigNode& internal_node, ProcessNode* parent_node) {
  fit::result<OutputNode*, zx_status_t> result;
  auto next_node_internal =
      GetNextNodeInPipeline(info->stream_config->properties.stream_type(), internal_node);
  if (!next_node_internal) {
    FX_LOGST(ERROR, kTag) << "Failed to get next node";
    return fit::error(ZX_ERR_INTERNAL);
  }

  switch (next_node_internal->type) {
    // Input Node
    case NodeType::kInputStream: {
      FX_LOGST(ERROR, kTag) << "Child node cannot be input node";
      return fit::error(ZX_ERR_INVALID_ARGS);
    }
    // GDC
    case NodeType::kGdc: {
      auto gdc_result = camera::GdcNode::CreateGdcNode(
          memory_allocator_, dispatcher_, device_, gdc_, info, parent_node, *next_node_internal);
      if (gdc_result.is_error()) {
        FX_PLOGST(ERROR, kTag, gdc_result.error()) << "Failed to configure GDC Node";
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
        FX_LOGST(ERROR, kTag) << "Failed to configure Output Node";
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

fit::result<std::unique_ptr<InputNode>, zx_status_t> PipelineManager::ConfigureStreamPipelineHelper(
    StreamCreationData* info, fidl::InterfaceRequest<fuchsia::camera2::Stream>& stream) {
  // Configure Input node
  auto input_result =
      camera::InputNode::CreateInputNode(info, memory_allocator_, dispatcher_, isp_);
  if (input_result.is_error()) {
    FX_PLOGST(ERROR, kTag, input_result.error()) << "Failed to ConfigureInputNode";
    return input_result;
  }

  auto input_processing_node = std::move(input_result.value());
  ProcessNode* input_node = input_processing_node.get();

  auto output_node_result = CreateGraph(info, info->node, input_node);
  if (output_node_result.is_error()) {
    FX_PLOGST(ERROR, kTag, output_node_result.error()) << "Failed to CreateGraph";
    return fit::error(output_node_result.error());
  }

  auto output_node = output_node_result.value();
  auto input_stream_type = info->node.input_stream_type;
  auto stream_configured = info->stream_config->properties.stream_type();

  auto status =
      output_node->Attach(stream.TakeChannel(), [this, input_stream_type, stream_configured]() {
        OnClientStreamDisconnect(input_stream_type, stream_configured);
      });
  if (status != ZX_OK) {
    FX_PLOGST(ERROR, kTag, status) << "Failed to bind output stream";
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
  for (auto& child_node : node->child_nodes()) {
    if (HasStreamType(child_node->supported_streams(), requested_stream_type)) {
      // If we find a child node which supports the requested stream type,
      // we move on to that child node.
      auto next_internal_node = GetNextNodeInPipeline(info->stream_config->properties.stream_type(),
                                                      current_internal_node);
      if (!next_internal_node) {
        FX_LOGS(ERROR) << "Failed to get next node for requested stream";
        return fit::error(ZX_ERR_INTERNAL);
      }
      return FindNodeToAttachNewStream(info, *next_internal_node, child_node.get());
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
  auto next_node_internal =
      GetNextNodeInPipeline(info->stream_config->properties.stream_type(), result.value().first);
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
  auto input_stream_type = info->node.input_stream_type;
  auto stream_configured = info->stream_config->properties.stream_type();
  auto status =
      output_node->Attach(stream.TakeChannel(), [this, input_stream_type, stream_configured]() {
        FX_LOGS(INFO) << "Stream client disconnected";
        OnClientStreamDisconnect(input_stream_type, stream_configured);
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
      fbl::AutoLock lock(&full_resolution_stream_lock_);
      if (full_resolution_stream_) {
        // If the same stream is requested again, we return failure.
        if (HasStreamType(full_resolution_stream_->configured_streams(),
                          info->stream_config->properties.stream_type())) {
          FX_PLOGS(ERROR, ZX_ERR_ALREADY_BOUND) << "Stream already bound";
          return ZX_ERR_ALREADY_BOUND;
        }
        // We will now append the requested stream to the existing graph.
        auto result = AppendToExistingGraph(info, full_resolution_stream_.get(), stream);
        if (result != ZX_OK) {
          FX_PLOGST(ERROR, kTag, result) << "AppendToExistingGraph failed";
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
      fbl::AutoLock lock(&downscaled_resolution_stream_lock_);
      if (downscaled_resolution_stream_) {
        // If the same stream is requested again, we return failure.
        if (HasStreamType(downscaled_resolution_stream_->configured_streams(),
                          info->stream_config->properties.stream_type())) {
          FX_PLOGST(ERROR, kTag, ZX_ERR_ALREADY_BOUND) << "Stream already bound";
          return ZX_ERR_ALREADY_BOUND;
        }
        // We will now append the requested stream to the existing graph.
        auto result = AppendToExistingGraph(info, downscaled_resolution_stream_.get(), stream);
        if (result != ZX_OK) {
          FX_PLOGST(ERROR, kTag, result) << "AppendToExistingGraph failed";
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
      FX_LOGST(ERROR, kTag) << "Invalid input stream type";
      return ZX_ERR_INVALID_ARGS;
    }
  }
  return ZX_OK;
}

static void RemoveStreamType(std::vector<fuchsia::camera2::CameraStreamType>& streams,
                             fuchsia::camera2::CameraStreamType stream_to_remove) {
  auto it = std::find(streams.begin(), streams.end(), stream_to_remove);
  if (it != streams.end()) {
    streams.erase(it);
  }
}

void PipelineManager::DeleteGraphForDisconnectedStream(
    ProcessNode* graph_head, fuchsia::camera2::CameraStreamType stream_to_disconnect) {
  // More than one stream supported by this graph.
  // Check for this nodes children to see if we can find the |stream_to_disconnect|
  // as part of configured_streams.
  auto& child_nodes = graph_head->child_nodes();
  for (auto it = child_nodes.begin(); it != child_nodes.end(); it++) {
    if (HasStreamType(it->get()->configured_streams(), stream_to_disconnect)) {
      if (it->get()->configured_streams().size() == 1) {
        it = child_nodes.erase(it);

        auto current_node = graph_head;
        while (current_node) {
          // Remove entry from configured streams.
          RemoveStreamType(current_node->configured_streams(), stream_to_disconnect);

          current_node = current_node->parent_node();
        }
        return;
      }
      return DeleteGraphForDisconnectedStream(it->get(), stream_to_disconnect);
    }
  }
}

void PipelineManager::DisconnectStream(ProcessNode* graph_head,
                                       fuchsia::camera2::CameraStreamType input_stream_type,
                                       fuchsia::camera2::CameraStreamType stream_to_disconnect) {
  auto shutdown_callback = [this, input_stream_type, stream_to_disconnect]() {
    ProcessNode* graph_head = nullptr;

    switch (input_stream_type) {
      case fuchsia::camera2::CameraStreamType::FULL_RESOLUTION: {
        fbl::AutoLock lock(&full_resolution_stream_lock_);
        ZX_ASSERT_MSG(full_resolution_stream_ != nullptr, "FR node is null");
        if (full_resolution_stream_->configured_streams().size() == 1) {
          full_resolution_stream_ = nullptr;
          return;
        }
        graph_head = full_resolution_stream_.get();
        DeleteGraphForDisconnectedStream(graph_head, stream_to_disconnect);
        break;
      }
      case fuchsia::camera2::CameraStreamType::DOWNSCALED_RESOLUTION: {
        fbl::AutoLock lock(&downscaled_resolution_stream_lock_);
        ZX_ASSERT_MSG(downscaled_resolution_stream_ != nullptr, "DS node is null");
        if (downscaled_resolution_stream_->configured_streams().size() == 1) {
          downscaled_resolution_stream_ = nullptr;
          return;
        }
        graph_head = downscaled_resolution_stream_.get();
        DeleteGraphForDisconnectedStream(graph_head, stream_to_disconnect);
        break;
      }
      default: {
        ZX_ASSERT_MSG(false, "Invalid input stream type\n");
      }
    }
  };

  // Only one stream supported by the graph.
  if (graph_head->configured_streams().size() == 1 &&
      HasStreamType(graph_head->configured_streams(), stream_to_disconnect)) {
    graph_head->OnShutdown(shutdown_callback);
    return;
  }

  // More than one stream supported by this graph.
  // Check for this nodes children to see if we can find the |stream_to_disconnect|
  // as part of configured_streams.
  auto& child_nodes = graph_head->child_nodes();
  for (auto& child_node : child_nodes) {
    if (HasStreamType(child_node->configured_streams(), stream_to_disconnect)) {
      return DisconnectStream(child_node.get(), input_stream_type, stream_to_disconnect);
    }
  }
}

void PipelineManager::OnClientStreamDisconnect(
    fuchsia::camera2::CameraStreamType input_stream_type,
    fuchsia::camera2::CameraStreamType stream_to_disconnect) {
  ProcessNode* graph_head = nullptr;
  switch (input_stream_type) {
    case fuchsia::camera2::CameraStreamType::FULL_RESOLUTION: {
      fbl::AutoLock lock(&full_resolution_stream_lock_);
      ZX_ASSERT_MSG(full_resolution_stream_ != nullptr, "FR node is null");
      graph_head = full_resolution_stream_.get();
      break;
    }
    case fuchsia::camera2::CameraStreamType::DOWNSCALED_RESOLUTION: {
      fbl::AutoLock lock(&downscaled_resolution_stream_lock_);
      ZX_ASSERT_MSG(downscaled_resolution_stream_ != nullptr, "DS node is null");
      graph_head = downscaled_resolution_stream_.get();
      break;
    }
    default: {
      ZX_ASSERT_MSG(false, "Invalid input stream type\n");
      return;
    }
  }
  DisconnectStream(graph_head, input_stream_type, stream_to_disconnect);
}

}  // namespace camera
