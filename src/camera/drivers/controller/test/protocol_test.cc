// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/fake_ddk/fake_ddk.h>
#include <lib/fit/function.h>
#include <lib/gtest/test_loop_fixture.h>
#include <lib/sys/cpp/component_context.h>
#include <zircon/errors.h>
#include <zircon/syscalls.h>
#include <zircon/types.h>

#include <fbl/auto_call.h>

#include "fake_gdc.h"
#include "fake_ge2d.h"
#include "fake_isp.h"
#include "lib/fit/result.h"
#include "src/camera/drivers/controller/configs/sherlock/sherlock_configs.h"
#include "src/camera/drivers/controller/controller-protocol.h"
#include "src/camera/drivers/controller/graph_utils.h"
#include "src/camera/drivers/controller/isp_stream_protocol.h"
#include "src/camera/drivers/controller/pipeline_manager.h"

// NOTE: In this test, we are actually just unit testing the ControllerImpl class.

namespace camera {

namespace {
constexpr uint32_t kDebugConfig = 0;
constexpr uint32_t kMonitorConfig = 1;
constexpr uint32_t kVideoConfig = 2;
constexpr auto kStreamTypeFR = fuchsia::camera2::CameraStreamType::FULL_RESOLUTION;
constexpr auto kStreamTypeDS = fuchsia::camera2::CameraStreamType::DOWNSCALED_RESOLUTION;
constexpr auto kStreamTypeML = fuchsia::camera2::CameraStreamType::MACHINE_LEARNING;
constexpr auto kStreamTypeVideo = fuchsia::camera2::CameraStreamType::VIDEO_CONFERENCE;
constexpr auto kStreamTypeMonitoring = fuchsia::camera2::CameraStreamType::MONITORING;
constexpr auto kNumBuffers = 5;

class ControllerProtocolTest : public gtest::TestLoopFixture {
 public:
  ControllerProtocolTest()
      : loop_(&kAsyncLoopConfigNoAttachToCurrentThread),
        context_(sys::ComponentContext::Create()) {}

  void SetUp() override {
    ASSERT_EQ(ZX_OK, context_->svc()->Connect(sysmem_allocator1_.NewRequest()));
    ASSERT_EQ(ZX_OK, context_->svc()->Connect(sysmem_allocator2_.NewRequest()));
    ASSERT_EQ(ZX_OK, loop_.StartThread("test-controller-frame-processing-thread",
                                       &controller_frame_processing_thread_));
    ASSERT_EQ(ZX_OK, zx::event::create(0, &event_));

    isp_ = fake_isp_.client();
    gdc_ = fake_gdc_.client();
    ge2d_ = fake_ge2d_.client();
    pipeline_manager_ =
        std::make_unique<PipelineManager>(fake_ddk::kFakeParent, loop_.dispatcher(), isp_, gdc_,
                                          ge2d_, std::move(sysmem_allocator1_), event_);
    internal_config_info_ = SherlockInternalConfigs();
  }

  void TearDown() override {
    pipeline_manager_ = nullptr;
    loop_.Shutdown();
    context_ = nullptr;
    sysmem_allocator1_ = nullptr;
    sysmem_allocator2_ = nullptr;
  }

  InternalConfigNode* GetStreamConfigNode(uint32_t config_type,
                                          const fuchsia::camera2::CameraStreamType stream_type) {
    InternalConfigInfo& config_info = internal_config_info_.configs_info.at(0);

    switch (config_type) {
      case kDebugConfig: {
        config_info = internal_config_info_.configs_info.at(0);
        break;
      }
      case kMonitorConfig: {
        config_info = internal_config_info_.configs_info.at(1);
        break;
      }
      case kVideoConfig: {
        config_info = internal_config_info_.configs_info.at(2);
        break;
      }
      default: {
        return nullptr;
      }
    }

    for (auto& stream_info : config_info.streams_info) {
      auto supported_streams = stream_info.supported_streams;
      if (std::find(supported_streams.begin(), supported_streams.end(), stream_type) !=
          supported_streams.end()) {
        return &stream_info;
      }
    }
    return nullptr;
  }

  // This helper API does the basic validation of an Input Node.
  fit::result<std::unique_ptr<camera::InputNode>, zx_status_t> GetInputNode(
      const ControllerMemoryAllocator& allocator,
      const fuchsia::camera2::CameraStreamType stream_type, StreamCreationData* info) {
    fuchsia::sysmem::BufferCollectionInfo_2 buffer_collection;
    buffer_collection.buffer_count = kNumBuffers;
    EXPECT_NE(nullptr, info);

    info->output_buffers = std::move(buffer_collection);
    info->image_format_index = 0;

    auto result = camera::InputNode::CreateInputNode(info, allocator, loop_.dispatcher(), isp_);
    EXPECT_TRUE(result.is_ok());

    EXPECT_NE(nullptr, result.value()->isp_stream_protocol());
    EXPECT_EQ(NodeType::kInputStream, result.value()->type());
    return result;
  }

  // Returns |true| if all |streams| are present in the
  // vector |streams_to_validate|.
  bool HasAllStreams(const std::vector<fuchsia::camera2::CameraStreamType>& streams_to_validate,
                     const std::vector<fuchsia::camera2::CameraStreamType>& streams) {
    if (streams_to_validate.size() != streams.size()) {
      return false;
    }
    for (auto stream : streams) {
      if (!camera::HasStreamType(streams_to_validate, stream)) {
        return false;
      }
    }
    return true;
  }

  void TestDebugStreamConfigNode() {
    EXPECT_NE(nullptr, GetStreamConfigNode(kDebugConfig, kStreamTypeFR));
    EXPECT_EQ(nullptr, GetStreamConfigNode(kDebugConfig, kStreamTypeDS));
  }

  void TestOutputNode() {
    auto stream_type = kStreamTypeFR;
    auto stream_config_node = GetStreamConfigNode(kDebugConfig, stream_type);
    ASSERT_NE(nullptr, stream_config_node);
    StreamCreationData info;
    fuchsia::camera2::hal::StreamConfig stream_config;
    stream_config.properties.set_stream_type(stream_type);
    info.stream_config = &stream_config;
    info.node = *stream_config_node;

    ControllerMemoryAllocator allocator(std::move(sysmem_allocator2_));

    // Testing successful creation of |OutputNode|.
    auto input_result = GetInputNode(allocator, stream_type, &info);
    auto output_result = OutputNode::CreateOutputNode(loop_.dispatcher(), &info,
                                                      input_result.value().get(), info.node);
    EXPECT_TRUE(output_result.is_ok());
    ASSERT_NE(nullptr, output_result.value());
    EXPECT_NE(nullptr, output_result.value()->client_stream());
    EXPECT_EQ(NodeType::kOutputStream, output_result.value()->type());

    // Passing invalid arguments.
    output_result =
        OutputNode::CreateOutputNode(nullptr, &info, input_result.value().get(), info.node);
    EXPECT_EQ(ZX_ERR_INVALID_ARGS, output_result.error());

    output_result = OutputNode::CreateOutputNode(loop_.dispatcher(), nullptr,
                                                 input_result.value().get(), info.node);
    EXPECT_EQ(ZX_ERR_INVALID_ARGS, output_result.error());

    output_result = OutputNode::CreateOutputNode(loop_.dispatcher(), &info, nullptr, info.node);
    EXPECT_EQ(ZX_ERR_INVALID_ARGS, output_result.error());
  }

  void TestGdcNode() {
    auto stream_config_node = GetStreamConfigNode(kMonitorConfig, kStreamTypeDS | kStreamTypeML);
    ASSERT_NE(nullptr, stream_config_node);
    StreamCreationData info;
    fuchsia::camera2::hal::StreamConfig stream_config;
    stream_config.properties.set_stream_type(kStreamTypeDS | kStreamTypeML);
    info.stream_config = &stream_config;
    info.node = *stream_config_node;
    ControllerMemoryAllocator allocator(std::move(sysmem_allocator2_));

    auto input_result = GetInputNode(allocator, kStreamTypeDS | kStreamTypeML, &info);
    // Testing successful creation of |GdcNode|.
    auto next_node_internal = GetNextNodeInPipeline(kStreamTypeDS | kStreamTypeML, info.node);
    ASSERT_NE(nullptr, next_node_internal);
    auto gdc_result =
        GdcNode::CreateGdcNode(allocator, loop_.dispatcher(), fake_ddk::kFakeParent, gdc_, &info,
                               input_result.value().get(), *next_node_internal);
    EXPECT_TRUE(gdc_result.is_ok());
    ASSERT_NE(nullptr, gdc_result.value());
    EXPECT_EQ(NodeType::kGdc, gdc_result.value()->type());
  }

  fit::result<std::pair<fidl::InterfaceRequest<fuchsia::camera2::Stream>, StreamCreationData>,
              zx_status_t>
  SetupStream(uint32_t config, fuchsia::camera2::CameraStreamType stream_type,
              fuchsia::camera2::StreamPtr& stream) {
    auto stream_config_node = GetStreamConfigNode(config, stream_type);
    if (stream_config_node == nullptr) {
      return fit::error(ZX_ERR_INVALID_ARGS);
    }
    StreamCreationData info;
    fuchsia::camera2::hal::StreamConfig stream_config;
    stream_config.properties.set_stream_type(stream_type);
    info.stream_config = &stream_config;
    info.node = *stream_config_node;
    fuchsia::sysmem::BufferCollectionInfo_2 buffer_collection;
    buffer_collection.buffer_count = kNumBuffers;
    info.output_buffers = std::move(buffer_collection);
    auto stream_request = stream.NewRequest();
    auto status = pipeline_manager_->ConfigureStreamPipeline(&info, stream_request);
    if (status != ZX_OK) {
      return fit::error(status);
    }
    auto result = std::make_pair(std::move(stream_request), std::move(info));
    return fit::ok(std::move(result));
  }

  void TestConfigureDebugConfig() {
    fuchsia::camera2::StreamPtr stream;
    auto stream_type = kStreamTypeFR;
    auto result = SetupStream(kDebugConfig, stream_type, stream);
    ASSERT_EQ(ZX_OK, result.error());

    auto fr_head_node = pipeline_manager_->full_resolution_stream();
    EXPECT_EQ(fr_head_node->type(), NodeType::kInputStream);
    EXPECT_TRUE(HasAllStreams(fr_head_node->configured_streams(), {stream_type}));
    EXPECT_TRUE(HasAllStreams(fr_head_node->supported_streams(), {stream_type}));

    auto output_node = static_cast<OutputNode*>(fr_head_node->child_nodes().at(0).get());
    EXPECT_EQ(output_node->type(), NodeType::kOutputStream);
    EXPECT_TRUE(HasAllStreams(output_node->configured_streams(), {stream_type}));
    EXPECT_TRUE(HasAllStreams(output_node->supported_streams(), {stream_type}));
    EXPECT_NE(nullptr, output_node->client_stream());
  }

  void TestConfigureMonitorConfigStreamFR() {
    fuchsia::camera2::StreamPtr stream;
    auto stream_type1 = kStreamTypeDS | kStreamTypeML;
    auto stream_type2 = kStreamTypeFR | kStreamTypeML;
    auto result = SetupStream(kMonitorConfig, stream_type2, stream);
    ASSERT_EQ(ZX_OK, result.error());

    auto fr_head_node = pipeline_manager_->full_resolution_stream();
    auto output_node = static_cast<OutputNode*>(fr_head_node->child_nodes().at(0).get());

    // Check if all nodes were created.
    EXPECT_EQ(NodeType::kInputStream, fr_head_node->type());
    EXPECT_EQ(NodeType::kOutputStream, output_node->type());

    // Validate the configured streams for all nodes.
    EXPECT_TRUE(HasAllStreams(fr_head_node->configured_streams(), {stream_type2}));
    EXPECT_TRUE(HasAllStreams(output_node->configured_streams(), {stream_type2}));

    EXPECT_TRUE(HasAllStreams(fr_head_node->supported_streams(), {stream_type1, stream_type2}));

    // Check if client_stream is valid.
    EXPECT_NE(nullptr, output_node->client_stream());
  }

  void TestConfigureMonitorConfigStreamDS() {
    auto stream_type1 = kStreamTypeDS | kStreamTypeML;
    auto stream_type2 = kStreamTypeFR | kStreamTypeML;
    fuchsia::camera2::StreamPtr stream;
    auto result = SetupStream(kMonitorConfig, stream_type1, stream);
    ASSERT_EQ(ZX_OK, result.error());

    auto fr_head_node = pipeline_manager_->full_resolution_stream();
    auto gdc_node = static_cast<GdcNode*>(fr_head_node->child_nodes().at(0).get());
    auto output_node = static_cast<OutputNode*>(gdc_node->child_nodes().at(0).get());

    // Check if all nodes were created.
    EXPECT_EQ(NodeType::kGdc, gdc_node->type());
    EXPECT_EQ(NodeType::kInputStream, fr_head_node->type());
    EXPECT_EQ(NodeType::kOutputStream, output_node->type());

    // Validate the configured streams for all nodes.
    EXPECT_TRUE(HasAllStreams(fr_head_node->configured_streams(), {stream_type1}));
    EXPECT_TRUE(HasAllStreams(gdc_node->configured_streams(), {stream_type1}));
    EXPECT_TRUE(HasAllStreams(output_node->configured_streams(), {stream_type1}));

    EXPECT_TRUE(HasAllStreams(fr_head_node->supported_streams(), {stream_type1, stream_type2}));
    EXPECT_TRUE(HasAllStreams(gdc_node->supported_streams(), {stream_type1}));
    EXPECT_TRUE(HasAllStreams(output_node->supported_streams(), {stream_type1}));

    // Check if client_stream is valid.
    EXPECT_NE(nullptr, output_node->client_stream());
  }

  void TestConfigure_MonitorConfig_MultiStreamFR() {
    fuchsia::camera2::StreamPtr stream1;
    fuchsia::camera2::StreamPtr stream2;

    auto stream_type1 = kStreamTypeDS | kStreamTypeML;
    auto stream_type2 = kStreamTypeFR | kStreamTypeML;

    auto result2 = SetupStream(kMonitorConfig, stream_type2, stream2);
    ASSERT_EQ(ZX_OK, result2.error());

    auto result1 = SetupStream(kMonitorConfig, stream_type1, stream1);
    ASSERT_EQ(ZX_OK, result1.error());

    auto fr_head_node = pipeline_manager_->full_resolution_stream();
    auto fr_ml_output_node = static_cast<OutputNode*>(fr_head_node->child_nodes().at(0).get());
    auto gdc_node = static_cast<GdcNode*>(fr_head_node->child_nodes().at(1).get());
    auto ds_ml_output_node = static_cast<OutputNode*>(gdc_node->child_nodes().at(0).get());

    // Validate input node.
    EXPECT_TRUE(HasAllStreams(fr_head_node->configured_streams(), {stream_type1, stream_type2}));
    EXPECT_TRUE(HasAllStreams(fr_head_node->supported_streams(), {stream_type1, stream_type2}));

    // Check if client_stream is valid.
    ASSERT_NE(nullptr, fr_ml_output_node->client_stream());
    ASSERT_NE(nullptr, ds_ml_output_node->client_stream());

    // Start streaming on FR|ML stream. Expecting other stream to be disabled.
    fr_ml_output_node->client_stream()->Start();
    EXPECT_TRUE(fr_head_node->enabled());
    EXPECT_TRUE(fr_ml_output_node->enabled());
    EXPECT_FALSE(gdc_node->enabled());
    EXPECT_FALSE(ds_ml_output_node->enabled());

    // Start streaming on DS|ML stream.
    ds_ml_output_node->client_stream()->Start();
    EXPECT_TRUE(fr_head_node->enabled());
    EXPECT_TRUE(fr_ml_output_node->enabled());
    EXPECT_TRUE(gdc_node->enabled());
    EXPECT_TRUE(ds_ml_output_node->enabled());

    // Stop streaming on FR|ML stream.
    fr_ml_output_node->client_stream()->Stop();
    EXPECT_TRUE(fr_head_node->enabled());
    EXPECT_FALSE(fr_ml_output_node->enabled());
    EXPECT_TRUE(gdc_node->enabled());
    EXPECT_TRUE(ds_ml_output_node->enabled());

    // Stop streaming on DS|ML stream.
    ds_ml_output_node->client_stream()->Stop();
    EXPECT_FALSE(fr_head_node->enabled());
    EXPECT_FALSE(fr_ml_output_node->enabled());
    EXPECT_FALSE(gdc_node->enabled());
    EXPECT_FALSE(ds_ml_output_node->enabled());
  }

  void TestConfigure_MonitorConfig_MultiStreamFR_BadOrdering() {
    auto stream_type1 = kStreamTypeDS | kStreamTypeML;
    auto stream_type2 = kStreamTypeFR | kStreamTypeML;
    fuchsia::camera2::StreamPtr stream1;
    fuchsia::camera2::StreamPtr stream2;

    auto result1 = SetupStream(kMonitorConfig, stream_type1, stream1);
    ASSERT_EQ(ZX_OK, result1.error());

    auto result2 = SetupStream(kMonitorConfig, stream_type2, stream2);
    EXPECT_EQ(ZX_ERR_NOT_SUPPORTED, result2.error());
  }

  void TestConfigureVideoConfigStream1() {
    auto stream_type = kStreamTypeFR | kStreamTypeML | kStreamTypeVideo;
    fuchsia::camera2::StreamPtr stream;
    auto result = SetupStream(kVideoConfig, stream_type, stream);
    ASSERT_EQ(ZX_OK, result.error());

    fuchsia::camera2::StreamPtr stream_video;
    auto video_result = SetupStream(kVideoConfig, kStreamTypeVideo, stream_video);
    ASSERT_EQ(ZX_OK, video_result.error());

    auto fr_head_node = pipeline_manager_->full_resolution_stream();
    auto gdc1_node = static_cast<GdcNode*>(fr_head_node->child_nodes().at(0).get());
    auto gdc2_node = static_cast<GdcNode*>(gdc1_node->child_nodes().at(0).get());
    auto output_node = static_cast<OutputNode*>(gdc2_node->child_nodes().at(0).get());
    auto ge2d_node = static_cast<Ge2dNode*>(gdc1_node->child_nodes().at(1).get());
    auto output_node_video = static_cast<OutputNode*>(ge2d_node->child_nodes().at(0).get());

    // Check if all nodes were created appropriately.
    EXPECT_EQ(NodeType::kGdc, gdc1_node->type());
    EXPECT_EQ(NodeType::kGdc, gdc2_node->type());
    EXPECT_EQ(NodeType::kGe2d, ge2d_node->type());
    EXPECT_EQ(NodeType::kInputStream, fr_head_node->type());
    EXPECT_EQ(NodeType::kOutputStream, output_node->type());
    EXPECT_EQ(NodeType::kOutputStream, output_node_video->type());

    // Validate the configured streams for all nodes.
    EXPECT_TRUE(HasAllStreams(fr_head_node->configured_streams(), {stream_type, kStreamTypeVideo}));
    EXPECT_TRUE(HasAllStreams(gdc1_node->configured_streams(), {stream_type, kStreamTypeVideo}));
    EXPECT_TRUE(HasAllStreams(gdc2_node->configured_streams(), {stream_type}));
    EXPECT_TRUE(HasAllStreams(ge2d_node->configured_streams(), {kStreamTypeVideo}));
    EXPECT_TRUE(HasAllStreams(output_node->configured_streams(), {stream_type}));
    EXPECT_TRUE(HasAllStreams(output_node_video->configured_streams(), {kStreamTypeVideo}));

    EXPECT_TRUE(HasAllStreams(fr_head_node->supported_streams(), {stream_type, kStreamTypeVideo}));
    EXPECT_TRUE(HasAllStreams(gdc1_node->supported_streams(), {stream_type, kStreamTypeVideo}));
    EXPECT_TRUE(HasAllStreams(gdc2_node->supported_streams(), {stream_type}));
    EXPECT_TRUE(HasAllStreams(ge2d_node->supported_streams(), {kStreamTypeVideo}));
    EXPECT_TRUE(HasAllStreams(output_node->supported_streams(), {stream_type}));
    EXPECT_TRUE(HasAllStreams(output_node_video->supported_streams(), {kStreamTypeVideo}));

    // Check if client_stream is valid.
    EXPECT_NE(nullptr, output_node->client_stream());
    EXPECT_NE(nullptr, output_node_video->client_stream());
  }

  void TestGdcConfigLoading() {
    auto result = camera::LoadGdcConfiguration(fake_ddk::kFakeParent, GdcConfig::INVALID);
    EXPECT_TRUE(result.is_error());

    result = camera::LoadGdcConfiguration(fake_ddk::kFakeParent, GdcConfig::MONITORING_360p);
    EXPECT_FALSE(result.is_error());
  }

  void TestHasStreamType() {
    std::vector<fuchsia::camera2::CameraStreamType> input_vector;
    auto stream_to_find = kStreamTypeFR;

    EXPECT_FALSE(HasStreamType(input_vector, stream_to_find));

    input_vector.push_back(kStreamTypeML);
    input_vector.push_back(kStreamTypeMonitoring);

    EXPECT_FALSE(HasStreamType(input_vector, stream_to_find));

    input_vector.push_back(kStreamTypeFR);
    EXPECT_TRUE(HasStreamType(input_vector, stream_to_find));
  }

  void TestGetNextNodeInPipeline() {
    auto stream_config_node = GetStreamConfigNode(kMonitorConfig, kStreamTypeDS | kStreamTypeML);
    ASSERT_NE(nullptr, stream_config_node);

    StreamCreationData info;
    fuchsia::camera2::hal::StreamConfig stream_config;
    stream_config.properties.set_stream_type(kStreamTypeDS | kStreamTypeML);
    info.stream_config = &stream_config;
    info.node = *stream_config_node;

    // Expecting 1st node to be input node.
    EXPECT_EQ(NodeType::kInputStream, stream_config_node->type);

    // Using ML|DS stream in Monitor configuration for test here.
    auto next_node = camera::GetNextNodeInPipeline(info.stream_config->properties.stream_type(),
                                                   *stream_config_node);
    ASSERT_NE(nullptr, next_node);

    // Expecting 2nd node to be input node.
    EXPECT_EQ(NodeType::kGdc, next_node->type);

    next_node =
        camera::GetNextNodeInPipeline(info.stream_config->properties.stream_type(), *next_node);
    ASSERT_NE(nullptr, next_node);

    // Expecting 3rd node to be input node.
    EXPECT_EQ(NodeType::kOutputStream, next_node->type);
  }

  void TestMultipleStartStreaming() {
    auto stream_type = kStreamTypeFR;
    fuchsia::camera2::StreamPtr stream;
    auto result = SetupStream(kDebugConfig, stream_type, stream);
    ASSERT_EQ(ZX_OK, result.error());

    auto fr_head_node = pipeline_manager_->full_resolution_stream();
    auto output_node = static_cast<OutputNode*>(fr_head_node->child_nodes().at(0).get());

    // Set streaming on.
    output_node->client_stream()->Start();
    EXPECT_NO_FATAL_FAILURE(output_node->client_stream()->Start());
  }

  void TestInUseBufferCounts() {
    auto stream_type = kStreamTypeFR | kStreamTypeML;
    fuchsia::camera2::StreamPtr stream;
    auto result = SetupStream(kMonitorConfig, stream_type, stream);
    ASSERT_EQ(ZX_OK, result.error());

    bool stream_alive = true;
    stream.set_error_handler([&](zx_status_t /*status*/) { stream_alive = false; });

    bool frame_received = false;
    stream.events().OnFrameAvailable = [&](fuchsia::camera2::FrameAvailableInfo info) {
      frame_received = true;
    };

    auto fr_head_node = pipeline_manager_->full_resolution_stream();

    // Start streaming.
    stream->Start();

    RunLoopUntilIdle();

    // ISP is single parent for two nodes.
    // Invoke OnFrameAvailable() for the ISP node. Buffer index = 1.
    frame_available_info_t frame_info = {
        .frame_status = FRAME_STATUS_OK,
        .buffer_id = 1,
        .metadata =
            {
                .timestamp = static_cast<uint64_t>(zx_clock_get_monotonic()),
                .image_format_index = 0,
                .input_buffer_index = 0,
            },
    };

    for (uint32_t i = 0; i < kNumBuffers; i++) {
      frame_info.buffer_id = i;
      EXPECT_NO_FATAL_FAILURE(fr_head_node->OnReadyToProcess(&frame_info));
    }

    while (!frame_received) {
      RunLoopUntilIdle();
    }

    EXPECT_TRUE(frame_received);

    EXPECT_EQ(fr_head_node->get_in_use_buffer_count(0), 0u);
    EXPECT_EQ(fr_head_node->get_in_use_buffer_count(1), 0u);
    EXPECT_EQ(fr_head_node->get_in_use_buffer_count(2), 1u);
    EXPECT_EQ(fr_head_node->get_in_use_buffer_count(3), 0u);

    stream->ReleaseFrame(2);
    RunLoopUntilIdle();

    EXPECT_EQ(fr_head_node->get_in_use_buffer_count(2), 0u);
    stream->Stop();
  }

  void TestReleaseAfterStopStreaming() {
    auto stream_type = kStreamTypeDS | kStreamTypeML;
    fuchsia::camera2::StreamPtr stream;
    auto result = SetupStream(kMonitorConfig, stream_type, stream);
    ASSERT_EQ(ZX_OK, result.error());

    // Start streaming.
    stream->Start();
    RunLoopUntilIdle();

    auto fr_head_node = pipeline_manager_->full_resolution_stream();
    auto gdc_node = static_cast<GdcNode*>(fr_head_node->child_nodes().at(0).get());
    auto ds_ml_output_node = static_cast<OutputNode*>(gdc_node->child_nodes().at(0).get());

    EXPECT_FALSE(fake_isp_.frame_released());

    EXPECT_TRUE(fr_head_node->enabled());
    EXPECT_TRUE(gdc_node->enabled());
    EXPECT_TRUE(ds_ml_output_node->enabled());

    // Stop streaming.
    stream->Stop();
    RunLoopUntilIdle();

    EXPECT_FALSE(fr_head_node->enabled());
    EXPECT_FALSE(gdc_node->enabled());
    EXPECT_FALSE(ds_ml_output_node->enabled());

    // Invoke OnFrameAvailable() for the ISP node. Buffer index = 1.
    frame_available_info_t frame_info = {
        .frame_status = FRAME_STATUS_OK,
        .buffer_id = 1,
        .metadata =
            {
                .timestamp = static_cast<uint64_t>(zx_clock_get_monotonic()),
                .image_format_index = 0,
                .input_buffer_index = 0,
            },
    };

    // Making a frame available to ISP node.
    // Expecting the frame to be released since node is disabled.
    EXPECT_NO_FATAL_FAILURE(fr_head_node->OnFrameAvailable(&frame_info));
    EXPECT_TRUE(fake_isp_.frame_released());

    // Making a frame available to GDC node.
    // Expecting the frame to be released since node is disabled.
    EXPECT_NO_FATAL_FAILURE(gdc_node->OnFrameAvailable(&frame_info));
    EXPECT_TRUE(fake_gdc_.frame_released());
  }

  void TestEnabledDisableStreaming() {
    fuchsia::camera2::StreamPtr stream_ds;
    fuchsia::camera2::StreamPtr stream_fr;

    auto stream_type_ds = kStreamTypeDS | kStreamTypeML;
    auto stream_type_fr = kStreamTypeFR | kStreamTypeML;

    auto result_fr = SetupStream(kMonitorConfig, stream_type_fr, stream_fr);
    ASSERT_EQ(ZX_OK, result_fr.error());

    auto result_ds = SetupStream(kMonitorConfig, stream_type_ds, stream_ds);
    ASSERT_EQ(ZX_OK, result_ds.error());

    // Start streaming.
    stream_fr->Start();
    stream_ds->Start();
    RunLoopUntilIdle();

    auto fr_head_node = pipeline_manager_->full_resolution_stream();
    auto fr_ml_output_node = static_cast<OutputNode*>(fr_head_node->child_nodes().at(0).get());
    auto gdc_node = static_cast<GdcNode*>(fr_head_node->child_nodes().at(1).get());
    auto ds_ml_output_node = static_cast<OutputNode*>(gdc_node->child_nodes().at(0).get());

    EXPECT_TRUE(fr_head_node->enabled());
    EXPECT_TRUE(fr_ml_output_node->enabled());
    EXPECT_TRUE(gdc_node->enabled());
    EXPECT_TRUE(ds_ml_output_node->enabled());

    pipeline_manager_->StopStreaming();

    EXPECT_FALSE(fr_head_node->enabled());
    EXPECT_FALSE(fr_ml_output_node->enabled());
    EXPECT_FALSE(gdc_node->enabled());
    EXPECT_FALSE(ds_ml_output_node->enabled());

    pipeline_manager_->StartStreaming();

    EXPECT_TRUE(fr_head_node->enabled());
    EXPECT_TRUE(fr_ml_output_node->enabled());
    EXPECT_TRUE(gdc_node->enabled());
    EXPECT_TRUE(ds_ml_output_node->enabled());
  }

  void TestMultipleFrameRates() {
    auto fr_stream_type = kStreamTypeFR | kStreamTypeML;
    auto ds_stream_type = kStreamTypeMonitoring;
    fuchsia::camera2::StreamPtr fr_stream;
    auto fr_result = SetupStream(kMonitorConfig, fr_stream_type, fr_stream);
    ASSERT_EQ(ZX_OK, fr_result.error());

    fuchsia::camera2::StreamPtr ds_stream;
    auto ds_result = SetupStream(kMonitorConfig, ds_stream_type, ds_stream);
    ASSERT_EQ(ZX_OK, ds_result.error());

    bool fr_stream_alive = true;
    fr_stream.set_error_handler([&](zx_status_t status) { fr_stream_alive = false; });

    bool fr_frame_received = false;
    uint32_t fr_frame_index = 0;
    fr_stream.events().OnFrameAvailable = [&](fuchsia::camera2::FrameAvailableInfo info) {
      fr_frame_received = true;
      fr_frame_index = info.buffer_id;
    };

    bool ds_stream_alive = true;
    ds_stream.set_error_handler([&](zx_status_t status) { ds_stream_alive = false; });

    bool ds_frame_received = false;
    uint32_t ds_frame_index = 0;
    uint32_t ds_frame_count = 0;
    ds_stream.events().OnFrameAvailable = [&](fuchsia::camera2::FrameAvailableInfo info) {
      ds_frame_received = true;
      ds_frame_index = info.buffer_id;
      ds_frame_count++;
    };

    auto fr_head_node = pipeline_manager_->full_resolution_stream();
    auto ds_head_node = pipeline_manager_->downscaled_resolution_stream();

    // Start streaming.
    fr_stream->Start();
    ds_stream->Start();

    RunLoopUntilIdle();

    // Invoke OnFrameAvailable() for the ISP node. Buffer index = 1.
    frame_available_info_t frame_info = {
        .frame_status = FRAME_STATUS_OK,
        .buffer_id = 0,
        .metadata =
            {
                .timestamp = static_cast<uint64_t>(zx_clock_get_monotonic()),
                .image_format_index = 0,
                .input_buffer_index = 0,
            },
    };

    for (uint32_t i = 0; i < kNumBuffers; i++) {
      frame_info.buffer_id = i;
      EXPECT_NO_FATAL_FAILURE(fr_head_node->OnReadyToProcess(&frame_info));
      EXPECT_NO_FATAL_FAILURE(ds_head_node->OnReadyToProcess(&frame_info));
    }

    while (!fr_frame_received) {
      RunLoopUntilIdle();
    }

    EXPECT_EQ(fr_frame_index, 2u);

    while (ds_frame_count != kNumBuffers) {
      RunLoopUntilIdle();
    }

    // Check if all frames are recieved.
    EXPECT_EQ(ds_frame_index, 4u);
    EXPECT_EQ(ds_frame_count, 5u);
  }

  void TestFindGraphHead() {
    auto fr_stream_type = kStreamTypeFR | kStreamTypeML;
    auto ds_stream_type = kStreamTypeMonitoring;
    fuchsia::camera2::StreamPtr fr_stream;
    auto fr_result = SetupStream(kMonitorConfig, fr_stream_type, fr_stream);
    ASSERT_EQ(ZX_OK, fr_result.error());

    fuchsia::camera2::StreamPtr ds_stream;
    auto ds_result = SetupStream(kMonitorConfig, ds_stream_type, ds_stream);
    ASSERT_EQ(ZX_OK, ds_result.error());

    auto result = pipeline_manager_->FindGraphHead(fr_stream_type);
    EXPECT_FALSE(result.is_error());
    EXPECT_EQ(fuchsia::camera2::CameraStreamType::FULL_RESOLUTION, result.value().second);

    result = pipeline_manager_->FindGraphHead(ds_stream_type);
    EXPECT_FALSE(result.is_error());
    EXPECT_EQ(fuchsia::camera2::CameraStreamType::DOWNSCALED_RESOLUTION, result.value().second);

    result = pipeline_manager_->FindGraphHead(kStreamTypeVideo);
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error(), ZX_ERR_BAD_STATE);
  }

  void TestPipelineManagerShutdown() {
    fuchsia::camera2::StreamPtr stream_ds;
    fuchsia::camera2::StreamPtr stream_fr;

    auto stream_type_ds = kStreamTypeDS | kStreamTypeML;
    auto stream_type_fr = kStreamTypeFR | kStreamTypeML;

    auto result_fr = SetupStream(kMonitorConfig, stream_type_fr, stream_fr);
    ASSERT_EQ(ZX_OK, result_fr.error());

    auto result_ds = SetupStream(kMonitorConfig, stream_type_ds, stream_ds);
    ASSERT_EQ(ZX_OK, result_ds.error());

    // Start streaming.
    stream_fr->Start();
    stream_ds->Start();
    RunLoopUntilIdle();

    async::PostTask(loop_.dispatcher(), [this]() { pipeline_manager_->Shutdown(); });

    zx_signals_t pending;
    event_.wait_one(kPipelineManagerSignalExitDone, zx::time::infinite(), &pending);

    RunLoopUntilIdle();

    EXPECT_EQ(nullptr, pipeline_manager_->full_resolution_stream());
    EXPECT_EQ(nullptr, pipeline_manager_->downscaled_resolution_stream());
  }

  FakeIsp fake_isp_;
  FakeGdc fake_gdc_;
  FakeGe2d fake_ge2d_;
  zx::event event_;
  async::Loop loop_;
  thrd_t controller_frame_processing_thread_;
  fuchsia::camera2::hal::ControllerSyncPtr camera_client_;
  std::unique_ptr<sys::ComponentContext> context_;
  std::unique_ptr<camera::PipelineManager> pipeline_manager_;
  fuchsia::sysmem::AllocatorSyncPtr sysmem_allocator1_;
  fuchsia::sysmem::AllocatorSyncPtr sysmem_allocator2_;
  ddk::IspProtocolClient isp_;
  ddk::GdcProtocolClient gdc_;
  ddk::Ge2dProtocolClient ge2d_;
  InternalConfigs internal_config_info_;
};

TEST_F(ControllerProtocolTest, GetDebugStreamConfig) { TestDebugStreamConfigNode(); }

TEST_F(ControllerProtocolTest, ConfigureOutputNodeDebugConfig) { TestConfigureDebugConfig(); }

TEST_F(ControllerProtocolTest, TestConfigureMonitorConfigStreamFR) {
  TestConfigureMonitorConfigStreamFR();
}

TEST_F(ControllerProtocolTest, TestConfigureMonitorConfigStreamDS) {
  TestConfigureMonitorConfigStreamDS();
}

TEST_F(ControllerProtocolTest, TestConfigureVideoConfigStream1) {
  TestConfigureVideoConfigStream1();
}

TEST_F(ControllerProtocolTest, TestHasStreamType) { TestHasStreamType(); }

TEST_F(ControllerProtocolTest, TestNextNodeInPipeline) { TestGetNextNodeInPipeline(); }

TEST_F(ControllerProtocolTest, TestMultipleStartStreaming) { TestMultipleStartStreaming(); }

TEST_F(ControllerProtocolTest, TestConfigure_MonitorConfig_MultiStreamFR_BadOrdering) {
  TestConfigure_MonitorConfig_MultiStreamFR_BadOrdering();
}

TEST_F(ControllerProtocolTest, TestConfigure_MonitorConfig_MultiStreamFR) {
  TestConfigure_MonitorConfig_MultiStreamFR();
}

TEST_F(ControllerProtocolTest, TestInUseBufferCounts) { TestInUseBufferCounts(); }

TEST_F(ControllerProtocolTest, TestOutputNode) { TestOutputNode(); }

TEST_F(ControllerProtocolTest, TestGdcNode) { TestGdcNode(); }

TEST_F(ControllerProtocolTest, TestReleaseAfterStopStreaming) { TestReleaseAfterStopStreaming(); }

TEST_F(ControllerProtocolTest, TestEnabledDisableStreaming) { TestEnabledDisableStreaming(); }

TEST_F(ControllerProtocolTest, TestMultipleFrameRates) { TestMultipleFrameRates(); }

TEST_F(ControllerProtocolTest, TestFindGraphHead) { TestFindGraphHead(); }

TEST_F(ControllerProtocolTest, TestPipelineManagerShutdown) { TestPipelineManagerShutdown(); }

TEST_F(ControllerProtocolTest, LoadGdcConfig) {
#ifdef INTERNAL_ACCESS
  TestGdcConfigLoading();
#else
  GTEST_SKIP();
#endif
}
}  // namespace

}  // namespace camera
