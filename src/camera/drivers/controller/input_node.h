// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CAMERA_DRIVERS_CONTROLLER_INPUT_NODE_H_
#define SRC_CAMERA_DRIVERS_CONTROLLER_INPUT_NODE_H_

#include <fuchsia/camera2/cpp/fidl.h>
#include <fuchsia/camera2/hal/cpp/fidl.h>
#include <zircon/assert.h>

#include <ddktl/protocol/isp.h>

#include "src/camera/drivers/controller/isp_stream_protocol.h"
#include "src/camera/drivers/controller/processing_node.h"
#include "src/camera/drivers/controller/stream_pipeline_info.h"
#include "src/camera/lib/format_conversion/buffer_collection_helper.h"
#include "src/camera/lib/format_conversion/format_conversion.h"

// |InputNode| represents a |ProcessNode| which would talk to the
// ISP driver.
namespace camera {

class InputNode : public ProcessNode {
 public:
  InputNode(std::vector<fuchsia::sysmem::ImageFormat_2> output_image_formats,
            fuchsia::sysmem::BufferCollectionInfo_2 output_buffer_collection,
            fuchsia::camera2::CameraStreamType current_stream_type,
            std::vector<fuchsia::camera2::CameraStreamType> supported_streams,
            async_dispatcher_t* dispatcher, const ddk::IspProtocolClient& isp)
      : ProcessNode(NodeType::kInputStream, output_image_formats,
                    std::move(output_buffer_collection), current_stream_type, supported_streams),
        dispatcher_(dispatcher),
        isp_frame_callback_{OnIspFrameAvailable, this},
        isp_(isp) {}

  ~InputNode() { OnShutdown(); }

  // Creates an |InputNode| object.
  // 1. Creates the ISP stream protocol
  // 2. Creates the requested ISP stream
  // 3. Allocates buffers if needed
  // Args:
  // |info| : StreamCreationData for the requested stream.
  // |memory_allocator| : Memory allocator object to allocate memory using sysmem.
  // |dispatcher| : Dispatcher on which GDC tasks can be queued up.
  // |isp| : ISP protocol to talk to the driver.
  static fit::result<std::unique_ptr<InputNode>, zx_status_t> CreateInputNode(
      StreamCreationData* info, const ControllerMemoryAllocator& memory_allocator,
      async_dispatcher_t* dispatcher, const ddk::IspProtocolClient& isp);

  const hw_accel_frame_callback_t* isp_frame_callback() { return &isp_frame_callback_; }

  std::unique_ptr<camera::IspStreamProtocol>& isp_stream_protocol() { return isp_stream_protocol_; }

  void set_isp_stream_protocol(std::unique_ptr<camera::IspStreamProtocol> isp_stream_protocol) {
    isp_stream_protocol_ = std::move(isp_stream_protocol);
  }

  // Notifies that a frame is ready to be sent to the client.
  void OnReadyToProcess(uint32_t buffer_index) override;

  // Releases the frame associated with | buffer_index |.
  void OnReleaseFrame(uint32_t buffer_index) override;

  // Shuts down the stream with ISP.
  void OnShutdown() override{};

  // Notifies that the client has requested to start streaming.
  void OnStartStreaming() override;

  // Notifies that the client has requested to stop streaming.
  void OnStopStreaming() override;

 private:
  // Notifies when a new frame is available from the ISP.
  static void OnIspFrameAvailable(void* ctx, const frame_available_info_t* info) {
    static_cast<ProcessNode*>(ctx)->OnFrameAvailable(info);
  }

  __UNUSED async_dispatcher_t* dispatcher_;
  // ISP Frame callback.
  hw_accel_frame_callback_t isp_frame_callback_;
  // ISP stream protocol.
  std::unique_ptr<IspStreamProtocol> isp_stream_protocol_;
  // Protocol to talk to ISP driver.
  ddk::IspProtocolClient isp_;
};

}  // namespace camera

#endif  // SRC_CAMERA_DRIVERS_CONTROLLER_INPUT_NODE_H_
