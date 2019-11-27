// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/camera/drivers/controller/controller-protocol.h"

#include <lib/fake_ddk/fake_ddk.h>
#include <lib/fit/function.h>
#include <lib/gtest/test_loop_fixture.h>
#include <lib/sys/cpp/component_context.h>

#include <fbl/auto_call.h>

#include "fake_gdc.h"
#include "fake_isp.h"
#include "src/camera/drivers/controller/pipeline_manager.h"

// NOTE: In this test, we are actually just unit testing the ControllerImpl class

namespace camera {

namespace {
constexpr uint32_t kDebugConfig = 0;
constexpr uint32_t kMonitorConfig = 1;
constexpr uint32_t kVideoConfig = 2;
}  // namespace

class ControllerProtocolTest : public gtest::TestLoopFixture {
 public:
  ControllerProtocolTest()
      : loop_(&kAsyncLoopConfigNoAttachToCurrentThread),
        context_(sys::ComponentContext::Create()) {}

  void SetUp() override {
    ASSERT_EQ(ZX_OK, loop_.StartThread("camera-controller-loop"));
    ASSERT_EQ(ZX_OK, context_->svc()->Connect(sysmem_allocator1_.NewRequest()));
    ASSERT_EQ(ZX_OK, context_->svc()->Connect(sysmem_allocator2_.NewRequest()));
    isp_ = fake_isp_.client();
    gdc_ = fake_gdc_.client();
    controller_protocol_device_ = std::make_unique<ControllerImpl>(
        fake_ddk::kFakeParent, isp_, gdc_, std::move(sysmem_allocator1_));
    pipeline_manager_ = std::make_unique<PipelineManager>(fake_ddk::kFakeParent, isp_, gdc_,
                                                          std::move(sysmem_allocator2_));
  }

  void TearDown() override {
    camera_client_ = nullptr;
    context_ = nullptr;
    sysmem_allocator1_ = nullptr;
    sysmem_allocator2_ = nullptr;
    controller_protocol_device_ = nullptr;
    loop_.Shutdown();
  }

  void TestInternalConfigs() {
    InternalConfigInfo* info = nullptr;

    // Invalid config index and pointer
    EXPECT_EQ(controller_protocol_device_->GetInternalConfiguration(0, &info), ZX_ERR_INVALID_ARGS);
    EXPECT_EQ(controller_protocol_device_->GetInternalConfiguration(0, nullptr),
              ZX_ERR_INVALID_ARGS);

    controller_protocol_device_->PopulateConfigurations();

    // Debug Configuration
    EXPECT_EQ(ZX_OK, controller_protocol_device_->GetInternalConfiguration(kDebugConfig, &info));
    EXPECT_EQ(info->streams_info.size(), 1u);
    // 1st stream is FR
    EXPECT_EQ(info->streams_info[0].input_stream_type,
              fuchsia::camera2::CameraStreamType::FULL_RESOLUTION);
    // FR supported streams
    EXPECT_EQ(info->streams_info[0].supported_streams.size(), 1u);
    EXPECT_EQ(info->streams_info[0].supported_streams[0],
              fuchsia::camera2::CameraStreamType::FULL_RESOLUTION);

    // Monitor Configuration
    EXPECT_EQ(ZX_OK, controller_protocol_device_->GetInternalConfiguration(kMonitorConfig, &info));
    EXPECT_EQ(info->streams_info.size(), 2u);

    // 1st Stream is FR
    EXPECT_EQ(info->streams_info[0].input_stream_type,
              fuchsia::camera2::CameraStreamType::FULL_RESOLUTION);

    // FR Supported streams
    EXPECT_EQ(info->streams_info[0].supported_streams.size(), 2u);
    EXPECT_EQ(info->streams_info[0].supported_streams[0],
              fuchsia::camera2::CameraStreamType::FULL_RESOLUTION |
                  fuchsia::camera2::CameraStreamType::MACHINE_LEARNING);
    EXPECT_EQ(info->streams_info[0].supported_streams[1],
              fuchsia::camera2::CameraStreamType::DOWNSCALED_RESOLUTION |
                  fuchsia::camera2::CameraStreamType::MACHINE_LEARNING);

    // 2nd Stream is DS
    EXPECT_EQ(info->streams_info[1].input_stream_type,
              fuchsia::camera2::CameraStreamType::DOWNSCALED_RESOLUTION);

    // DS supported streams
    EXPECT_EQ(info->streams_info[1].supported_streams.size(), 1u);
    EXPECT_EQ(info->streams_info[1].supported_streams[0],
              fuchsia::camera2::CameraStreamType::MONITORING);
    EXPECT_EQ(info->streams_info[1].child_nodes[0].type, kGdc);
    ASSERT_EQ(info->streams_info[1].child_nodes[0].gdc_info.config_type.size(), 3u);
    EXPECT_EQ(info->streams_info[1].child_nodes[0].gdc_info.config_type[0],
              GdcConfig::MONITORING_720p);
    EXPECT_EQ(info->streams_info[1].child_nodes[0].gdc_info.config_type[1],
              GdcConfig::MONITORING_480p);
    EXPECT_EQ(info->streams_info[1].child_nodes[0].gdc_info.config_type[2],
              GdcConfig::MONITORING_360p);
    ASSERT_EQ(info->streams_info[1].child_nodes[0].supported_streams.size(), 1u);
    EXPECT_EQ(info->streams_info[1].child_nodes[0].supported_streams[0],
              fuchsia::camera2::CameraStreamType::MONITORING);

    // Output node.
    EXPECT_EQ(info->streams_info[1].child_nodes[0].child_nodes.size(), 1u);
    EXPECT_EQ(info->streams_info[1].child_nodes[0].child_nodes[0].type, kOutputStream);
    ASSERT_EQ(info->streams_info[1].child_nodes[0].child_nodes[0].supported_streams.size(), 1u);
    EXPECT_EQ(info->streams_info[1].child_nodes[0].child_nodes[0].supported_streams[0],
              fuchsia::camera2::CameraStreamType::MONITORING);

    // Video Conferencing configuration
    EXPECT_EQ(ZX_OK, controller_protocol_device_->GetInternalConfiguration(kVideoConfig, &info));
    EXPECT_EQ(info->streams_info.size(), 1u);

    // 1st stream is FR
    EXPECT_EQ(info->streams_info[0].input_stream_type,
              fuchsia::camera2::CameraStreamType::FULL_RESOLUTION);
    // FR Supported streams
    EXPECT_EQ(info->streams_info[0].supported_streams.size(), 2u);
    EXPECT_EQ(info->streams_info[0].supported_streams[0],
              fuchsia::camera2::CameraStreamType::FULL_RESOLUTION |
                  fuchsia::camera2::CameraStreamType::MACHINE_LEARNING |
                  fuchsia::camera2::CameraStreamType::VIDEO_CONFERENCE);
    EXPECT_EQ(info->streams_info[0].supported_streams[1],
              fuchsia::camera2::CameraStreamType::VIDEO_CONFERENCE);
  }

  void TestDebugStreamConfigNode() {
    controller_protocol_device_->PopulateConfigurations();
    InternalConfigInfo* info = nullptr;
    // Get internal configuration for debug config
    EXPECT_EQ(ZX_OK, controller_protocol_device_->GetInternalConfiguration(kDebugConfig, &info));
    // Get stream config for fuchsia::camera2::CameraStreamType::FULL_RESOLUTION; stream
    auto stream_config = controller_protocol_device_->GetStreamConfigNode(
        info, fuchsia::camera2::CameraStreamType::FULL_RESOLUTION);
    EXPECT_NE(nullptr, stream_config);

    stream_config = controller_protocol_device_->GetStreamConfigNode(
        info, fuchsia::camera2::CameraStreamType::DOWNSCALED_RESOLUTION);
    EXPECT_EQ(nullptr, stream_config);
  }

  void TestConfigureInputNode_DebugConfig() {
    controller_protocol_device_->PopulateConfigurations();
    InternalConfigInfo* internal_info = nullptr;
    // Get internal configuration for debug config
    EXPECT_EQ(ZX_OK,
              controller_protocol_device_->GetInternalConfiguration(kDebugConfig, &internal_info));
    // Get stream config for fuchsia::camera2::CameraStreamType::FULL_RESOLUTION; stream
    auto stream_config_node = controller_protocol_device_->GetStreamConfigNode(
        internal_info, fuchsia::camera2::CameraStreamType::FULL_RESOLUTION);
    ASSERT_NE(nullptr, stream_config_node);

    fuchsia::sysmem::BufferCollectionInfo_2 buffer_collection;
    fuchsia::camera2::hal::StreamConfig stream_config;
    stream_config.properties.set_stream_type(fuchsia::camera2::CameraStreamType::FULL_RESOLUTION);

    PipelineInfo info;
    info.output_buffers = std::move(buffer_collection);
    info.image_format_index = 0;
    info.node = *stream_config_node;
    info.stream_config = &stream_config;

    auto result = pipeline_manager_->CreateInputNode(&info);
    EXPECT_EQ(true, result.is_ok());

    EXPECT_NE(nullptr, result.value()->isp_stream_protocol());
    EXPECT_EQ(NodeType::kInputStream, result.value()->type());
  }

  void TestConfigureOutputNode_DebugConfig() {
    controller_protocol_device_->PopulateConfigurations();
    InternalConfigInfo* internal_info = nullptr;
    // Get internal configuration for debug config
    EXPECT_EQ(ZX_OK,
              controller_protocol_device_->GetInternalConfiguration(kDebugConfig, &internal_info));
    // Get stream config for fuchsia::camera2::CameraStreamType::FULL_RESOLUTION; stream
    auto stream_config_node = controller_protocol_device_->GetStreamConfigNode(
        internal_info, fuchsia::camera2::CameraStreamType::FULL_RESOLUTION);
    ASSERT_NE(nullptr, stream_config_node);

    fuchsia::sysmem::BufferCollectionInfo_2 buffer_collection;
    fuchsia::camera2::hal::StreamConfig stream_config;
    stream_config.properties.set_stream_type(fuchsia::camera2::CameraStreamType::FULL_RESOLUTION);

    PipelineInfo info;
    info.output_buffers = std::move(buffer_collection);
    info.image_format_index = 0;
    info.node = *stream_config_node;
    info.stream_config = &stream_config;

    auto result = pipeline_manager_->CreateInputNode(&info);
    ASSERT_EQ(true, result.is_ok());

    EXPECT_NE(nullptr, result.value()->isp_stream_protocol());
    EXPECT_EQ(NodeType::kInputStream, result.value()->type());

    auto graph_result = pipeline_manager_->CreateGraph(&info, info.node, result.value().get());
    ASSERT_EQ(true, graph_result.is_ok());

    ASSERT_NE(nullptr, graph_result.value());
    EXPECT_NE(nullptr, graph_result.value()->client_stream());
    EXPECT_EQ(NodeType::kOutputStream, graph_result.value()->type());
  }

  void TestConfigure_MonitorConfig_Stream1() {
    controller_protocol_device_->PopulateConfigurations();
    InternalConfigInfo* internal_info = nullptr;
    // Get the internal configuration for monitor config
    EXPECT_EQ(ZX_OK, controller_protocol_device_->GetInternalConfiguration(kMonitorConfig,
                                                                           &internal_info));
    // Get the stream config
    auto stream_config_node = controller_protocol_device_->GetStreamConfigNode(
        internal_info, fuchsia::camera2::CameraStreamType::DOWNSCALED_RESOLUTION |
                           fuchsia::camera2::CameraStreamType::MACHINE_LEARNING);
    ASSERT_NE(nullptr, stream_config_node);

    fuchsia::sysmem::BufferCollectionInfo_2 buffer_collection;
    fuchsia::camera2::hal::StreamConfig stream_config;
    stream_config.properties.set_stream_type(
        fuchsia::camera2::CameraStreamType::DOWNSCALED_RESOLUTION |
        fuchsia::camera2::CameraStreamType::MACHINE_LEARNING);

    PipelineInfo info;
    info.output_buffers = std::move(buffer_collection);
    info.image_format_index = 0;
    info.node = *stream_config_node;
    info.stream_config = &stream_config;

    auto result = pipeline_manager_->CreateInputNode(&info);
    EXPECT_EQ(true, result.is_ok());

    EXPECT_NE(nullptr, result.value()->isp_stream_protocol());
    EXPECT_EQ(NodeType::kInputStream, result.value()->type());

    auto graph_result = pipeline_manager_->CreateGraph(&info, info.node, result.value().get());
    ASSERT_EQ(true, graph_result.is_ok());

    ASSERT_NE(nullptr, graph_result.value());
    EXPECT_NE(nullptr, graph_result.value()->client_stream());
    EXPECT_EQ(NodeType::kOutputStream, graph_result.value()->type());

    // Check if GDC node was created.
    EXPECT_EQ(NodeType::kGdc, graph_result.value()->parent_node()->type());

    // Validate the configured and supported streams for Input node.
    ASSERT_EQ(result.value()->configured_streams().size(), 1u);
    EXPECT_EQ(result.value()->configured_streams().at(0),
              fuchsia::camera2::CameraStreamType::DOWNSCALED_RESOLUTION |
                  fuchsia::camera2::CameraStreamType::MACHINE_LEARNING);

    ASSERT_EQ(result.value()->supported_streams().size(), 2u);
    EXPECT_EQ(result.value()->supported_streams().at(0),
              fuchsia::camera2::CameraStreamType::FULL_RESOLUTION |
                  fuchsia::camera2::CameraStreamType::MACHINE_LEARNING);
    EXPECT_EQ(result.value()->supported_streams().at(1),
              fuchsia::camera2::CameraStreamType::DOWNSCALED_RESOLUTION |
                  fuchsia::camera2::CameraStreamType::MACHINE_LEARNING);

    // Validate the configured and supported streams for GDC node.
    ASSERT_EQ(graph_result.value()->parent_node()->configured_streams().size(), 1u);
    EXPECT_EQ(graph_result.value()->parent_node()->configured_streams().at(0),
              fuchsia::camera2::CameraStreamType::DOWNSCALED_RESOLUTION |
                  fuchsia::camera2::CameraStreamType::MACHINE_LEARNING);

    ASSERT_EQ(graph_result.value()->parent_node()->supported_streams().size(), 1u);
    EXPECT_EQ(graph_result.value()->parent_node()->supported_streams().at(0),
              fuchsia::camera2::CameraStreamType::DOWNSCALED_RESOLUTION |
                  fuchsia::camera2::CameraStreamType::MACHINE_LEARNING);

    // Validate the configured and supported streams for Output node.
    ASSERT_EQ(graph_result.value()->configured_streams().size(), 1u);
    EXPECT_EQ(graph_result.value()->configured_streams().at(0),
              fuchsia::camera2::CameraStreamType::DOWNSCALED_RESOLUTION |
                  fuchsia::camera2::CameraStreamType::MACHINE_LEARNING);

    ASSERT_EQ(graph_result.value()->supported_streams().size(), 1u);
    EXPECT_EQ(graph_result.value()->supported_streams().at(0),
              fuchsia::camera2::CameraStreamType::DOWNSCALED_RESOLUTION |
                  fuchsia::camera2::CameraStreamType::MACHINE_LEARNING);
  }

  void TestConfigure_MonitorConfig_Stream2() {
    controller_protocol_device_->PopulateConfigurations();
    InternalConfigInfo* internal_info = nullptr;
    // Get the internal configuration for monitor config
    EXPECT_EQ(ZX_OK, controller_protocol_device_->GetInternalConfiguration(kMonitorConfig,
                                                                           &internal_info));
    // Get the stream config
    auto stream_config_node = controller_protocol_device_->GetStreamConfigNode(
        internal_info, fuchsia::camera2::CameraStreamType::MONITORING);
    ASSERT_NE(nullptr, stream_config_node);

    fuchsia::sysmem::BufferCollectionInfo_2 buffer_collection;
    fuchsia::camera2::hal::StreamConfig stream_config;
    stream_config.properties.set_stream_type(fuchsia::camera2::CameraStreamType::MONITORING);

    PipelineInfo info;
    info.output_buffers = std::move(buffer_collection);
    info.image_format_index = 0;

    info.node = *stream_config_node;
    info.stream_config = &stream_config;

    auto result = pipeline_manager_->CreateInputNode(&info);
    EXPECT_EQ(true, result.is_ok());

    EXPECT_NE(nullptr, result.value()->isp_stream_protocol());
    EXPECT_EQ(NodeType::kInputStream, result.value()->type());

    auto graph_result = pipeline_manager_->CreateGraph(&info, info.node, result.value().get());
    ASSERT_EQ(true, graph_result.is_ok());

    ASSERT_NE(nullptr, graph_result.value());
    EXPECT_NE(nullptr, graph_result.value()->client_stream());
    EXPECT_EQ(NodeType::kOutputStream, graph_result.value()->type());

    // Check if GDC node was created.
    EXPECT_EQ(NodeType::kGdc, graph_result.value()->parent_node()->type());

    // Validate the configured and supported streams for Input node.
    ASSERT_EQ(result.value()->configured_streams().size(), 1u);
    EXPECT_EQ(result.value()->configured_streams().at(0),
              fuchsia::camera2::CameraStreamType::MONITORING);

    ASSERT_EQ(result.value()->supported_streams().size(), 1u);
    EXPECT_EQ(result.value()->supported_streams().at(0),
              fuchsia::camera2::CameraStreamType::MONITORING);

    // Validate the configured and supported streams for GDC node.
    ASSERT_EQ(graph_result.value()->parent_node()->configured_streams().size(), 1u);
    EXPECT_EQ(graph_result.value()->parent_node()->configured_streams().at(0),
              fuchsia::camera2::CameraStreamType::MONITORING);

    ASSERT_EQ(graph_result.value()->parent_node()->supported_streams().size(), 1u);
    EXPECT_EQ(graph_result.value()->parent_node()->supported_streams().at(0),
              fuchsia::camera2::CameraStreamType::MONITORING);

    // Validate the configured and supported streams for Output node.
    ASSERT_EQ(graph_result.value()->configured_streams().size(), 1u);
    EXPECT_EQ(graph_result.value()->configured_streams().at(0),
              fuchsia::camera2::CameraStreamType::MONITORING);

    ASSERT_EQ(graph_result.value()->supported_streams().size(), 1u);
    EXPECT_EQ(graph_result.value()->supported_streams().at(0),
              fuchsia::camera2::CameraStreamType::MONITORING);
  }

  void TestConfigure_VideoConfig_Stream1() {
    controller_protocol_device_->PopulateConfigurations();
    InternalConfigInfo* internal_info = nullptr;
    // Get the internal configuration for monitor config
    EXPECT_EQ(ZX_OK,
              controller_protocol_device_->GetInternalConfiguration(kVideoConfig, &internal_info));
    // Get the stream config
    auto stream_config_node = controller_protocol_device_->GetStreamConfigNode(
        internal_info, fuchsia::camera2::CameraStreamType::FULL_RESOLUTION |
                           fuchsia::camera2::CameraStreamType::MACHINE_LEARNING |
                           fuchsia::camera2::CameraStreamType::VIDEO_CONFERENCE);
    ASSERT_NE(nullptr, stream_config_node);

    fuchsia::sysmem::BufferCollectionInfo_2 buffer_collection;
    fuchsia::camera2::hal::StreamConfig stream_config;
    stream_config.properties.set_stream_type(fuchsia::camera2::CameraStreamType::FULL_RESOLUTION |
                                             fuchsia::camera2::CameraStreamType::MACHINE_LEARNING |
                                             fuchsia::camera2::CameraStreamType::VIDEO_CONFERENCE);

    PipelineInfo info;
    info.output_buffers = std::move(buffer_collection);
    info.image_format_index = 0;
    info.node = *stream_config_node;
    info.stream_config = &stream_config;

    auto result = pipeline_manager_->CreateInputNode(&info);
    EXPECT_EQ(true, result.is_ok());

    EXPECT_NE(nullptr, result.value()->isp_stream_protocol());
    EXPECT_EQ(NodeType::kInputStream, result.value()->type());

    auto graph_result = pipeline_manager_->CreateGraph(&info, info.node, result.value().get());
    ASSERT_EQ(true, graph_result.is_ok());

    ASSERT_NE(nullptr, graph_result.value());
    EXPECT_NE(nullptr, graph_result.value()->client_stream());
    EXPECT_EQ(NodeType::kOutputStream, graph_result.value()->type());

    // Check if GDC1 & GDC2 node was created.
    EXPECT_EQ(NodeType::kGdc, graph_result.value()->parent_node()->type());
    EXPECT_EQ(NodeType::kGdc, graph_result.value()->parent_node()->parent_node()->type());

    // Validate the configured and supported streams for Input node.
    ASSERT_EQ(result.value()->configured_streams().size(), 1u);
    EXPECT_EQ(result.value()->configured_streams().at(0),
              fuchsia::camera2::CameraStreamType::FULL_RESOLUTION |
                  fuchsia::camera2::CameraStreamType::MACHINE_LEARNING |
                  fuchsia::camera2::CameraStreamType::VIDEO_CONFERENCE);

    ASSERT_EQ(result.value()->supported_streams().size(), 2u);
    EXPECT_EQ(result.value()->supported_streams().at(0),
              fuchsia::camera2::CameraStreamType::FULL_RESOLUTION |
                  fuchsia::camera2::CameraStreamType::MACHINE_LEARNING |
                  fuchsia::camera2::CameraStreamType::VIDEO_CONFERENCE);
    EXPECT_EQ(result.value()->supported_streams().at(1),
              fuchsia::camera2::CameraStreamType::VIDEO_CONFERENCE);

    // Validate the configured and supported streams for GDC2 node.
    ASSERT_EQ(graph_result.value()->parent_node()->configured_streams().size(), 1u);
    EXPECT_EQ(graph_result.value()->parent_node()->configured_streams().at(0),
              fuchsia::camera2::CameraStreamType::FULL_RESOLUTION |
                  fuchsia::camera2::CameraStreamType::MACHINE_LEARNING |
                  fuchsia::camera2::CameraStreamType::VIDEO_CONFERENCE);

    ASSERT_EQ(graph_result.value()->parent_node()->supported_streams().size(), 1u);
    EXPECT_EQ(graph_result.value()->parent_node()->supported_streams().at(0),
              fuchsia::camera2::CameraStreamType::FULL_RESOLUTION |
                  fuchsia::camera2::CameraStreamType::MACHINE_LEARNING |
                  fuchsia::camera2::CameraStreamType::VIDEO_CONFERENCE);

    // Validate the configured and supported streams for GDC1 node.
    ASSERT_EQ(graph_result.value()->parent_node()->parent_node()->configured_streams().size(), 1u);
    EXPECT_EQ(graph_result.value()->parent_node()->parent_node()->configured_streams().at(0),
              fuchsia::camera2::CameraStreamType::FULL_RESOLUTION |
                  fuchsia::camera2::CameraStreamType::MACHINE_LEARNING |
                  fuchsia::camera2::CameraStreamType::VIDEO_CONFERENCE);

    ASSERT_EQ(graph_result.value()->parent_node()->parent_node()->supported_streams().size(), 2u);
    EXPECT_EQ(graph_result.value()->parent_node()->parent_node()->supported_streams().at(0),
              fuchsia::camera2::CameraStreamType::FULL_RESOLUTION |
                  fuchsia::camera2::CameraStreamType::MACHINE_LEARNING |
                  fuchsia::camera2::CameraStreamType::VIDEO_CONFERENCE);
    EXPECT_EQ(graph_result.value()->parent_node()->parent_node()->supported_streams().at(1),
              fuchsia::camera2::CameraStreamType::VIDEO_CONFERENCE);

    // Validate the configured and supported streams for Output node.
    ASSERT_EQ(graph_result.value()->configured_streams().size(), 1u);
    EXPECT_EQ(graph_result.value()->configured_streams().at(0),
              fuchsia::camera2::CameraStreamType::FULL_RESOLUTION |
                  fuchsia::camera2::CameraStreamType::MACHINE_LEARNING |
                  fuchsia::camera2::CameraStreamType::VIDEO_CONFERENCE);

    ASSERT_EQ(graph_result.value()->supported_streams().size(), 1u);
    EXPECT_EQ(graph_result.value()->supported_streams().at(0),
              fuchsia::camera2::CameraStreamType::FULL_RESOLUTION |
                  fuchsia::camera2::CameraStreamType::MACHINE_LEARNING |
                  fuchsia::camera2::CameraStreamType::VIDEO_CONFERENCE);
  }

  void TestShutdownPathAfterStreamingOn() {
    controller_protocol_device_->PopulateConfigurations();
    InternalConfigInfo* internal_info = nullptr;
    // Get internal configuration for debug config
    EXPECT_EQ(ZX_OK,
              controller_protocol_device_->GetInternalConfiguration(kDebugConfig, &internal_info));
    // Get stream config for fuchsia::camera2::CameraStreamType::FULL_RESOLUTION; stream
    auto stream_config_node = controller_protocol_device_->GetStreamConfigNode(
        internal_info, fuchsia::camera2::CameraStreamType::FULL_RESOLUTION);
    ASSERT_NE(nullptr, stream_config_node);

    fuchsia::sysmem::BufferCollectionInfo_2 buffer_collection;
    fuchsia::camera2::hal::StreamConfig stream_config;
    stream_config.properties.set_stream_type(fuchsia::camera2::CameraStreamType::FULL_RESOLUTION);

    PipelineInfo info;
    info.output_buffers = std::move(buffer_collection);
    info.image_format_index = 0;
    info.node = *stream_config_node;
    info.stream_config = &stream_config;

    auto result = pipeline_manager_->CreateInputNode(&info);
    EXPECT_EQ(true, result.is_ok());

    EXPECT_NE(nullptr, result.value()->isp_stream_protocol());
    EXPECT_EQ(NodeType::kInputStream, result.value()->type());

    auto graph_result = pipeline_manager_->CreateGraph(&info, info.node, result.value().get());
    EXPECT_EQ(true, graph_result.is_ok());

    ASSERT_NE(nullptr, graph_result.value());
    EXPECT_NE(nullptr, graph_result.value()->client_stream());
    EXPECT_EQ(NodeType::kOutputStream, graph_result.value()->type());

    // Set streaming on
    graph_result.value()->client_stream()->Start();

    EXPECT_NO_FATAL_FAILURE(pipeline_manager_->OnClientStreamDisconnect(&info));
  }

  void TestGdcConfigLoading() {
    auto result = pipeline_manager_->LoadGdcConfiguration(GdcConfig::INVALID);
    EXPECT_TRUE(result.is_error());

    result = pipeline_manager_->LoadGdcConfiguration(GdcConfig::MONITORING_360p);
    EXPECT_FALSE(result.is_error());
  }

  FakeIsp fake_isp_;
  FakeGdc fake_gdc_;
  async::Loop loop_;
  std::unique_ptr<ControllerImpl> controller_protocol_device_;
  fuchsia::camera2::hal::ControllerSyncPtr camera_client_;
  std::unique_ptr<sys::ComponentContext> context_;
  std::unique_ptr<camera::PipelineManager> pipeline_manager_;
  fuchsia::sysmem::AllocatorSyncPtr sysmem_allocator1_;
  fuchsia::sysmem::AllocatorSyncPtr sysmem_allocator2_;
  ddk::IspProtocolClient isp_;
  ddk::GdcProtocolClient gdc_;
};

TEST_F(ControllerProtocolTest, GetConfigs) { TestInternalConfigs(); }

TEST_F(ControllerProtocolTest, GetDebugStreamConfig) { TestDebugStreamConfigNode(); }

TEST_F(ControllerProtocolTest, ConfigureInputNodeDebugConfig) {
  TestConfigureInputNode_DebugConfig();
}

TEST_F(ControllerProtocolTest, ConfigureOutputNodeDebugConfig) {
  TestConfigureOutputNode_DebugConfig();
}

TEST_F(ControllerProtocolTest, TestShutdownPathAfterStreamingOn) {
  TestShutdownPathAfterStreamingOn();
}

TEST_F(ControllerProtocolTest, TestConfigure_MonitorConfig_Stream1) {
  TestConfigure_MonitorConfig_Stream1();
}

TEST_F(ControllerProtocolTest, TestConfigure_MonitorConfig_Stream2) {
  TestConfigure_MonitorConfig_Stream2();
}

TEST_F(ControllerProtocolTest, TestConfigure_VideoConfig_Stream1) {
  TestConfigure_VideoConfig_Stream1();
}

TEST_F(ControllerProtocolTest, LoadGdcConfig) {
#ifdef INTERNAL_ACCESS
  TestGdcConfigLoading();
#else
  GTEST_SKIP();
#endif
}

}  // namespace camera
