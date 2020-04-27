// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <fuchsia/gpu/magma/c/fidl.h>
#include <lib/fdio/unsafe.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include <gtest/gtest.h>

#include "helper/test_device_helper.h"
#include "magma_common_defs.h"
#include "src/graphics/tests/common/utils.h"
#include "src/graphics/tests/common/vulkan_context.h"

#include <vulkan/vulkan.hpp>

namespace {

class VkLoopTest {
 public:
  explicit VkLoopTest(bool hang_on_event) : hang_on_event_(hang_on_event) {}

  bool Initialize();
  bool Exec(bool kill_driver);

 private:
  bool InitBuffer();
  bool InitCommandBuffer();

  bool hang_on_event_;
  bool is_initialized_ = false;
  std::unique_ptr<VulkanContext> ctx_;
  VkDescriptorSet vk_descriptor_set_;
  VkPipelineLayout vk_pipeline_layout_;
  VkPipeline vk_compute_pipeline_;
  VkEvent vk_event_;
  vk::UniqueBuffer buffer_;
  vk::UniqueDeviceMemory buffer_memory_;
  vk::UniqueCommandPool command_pool_;
  std::vector<vk::UniqueCommandBuffer> command_buffers_;
};

bool VkLoopTest::Initialize() {
  if (is_initialized_) {
    return false;
  }

  ctx_ = VulkanContext::Builder{}.set_queue_flag_bits(vk::QueueFlagBits::eCompute).Unique();
  if (!ctx_) {
    RTN_MSG(false, "Failed to initialize Vulkan.\n");
  }

  if (!InitBuffer()) {
    RTN_MSG(false, "Failed to init buffer.\n");
  }

  if (!InitCommandBuffer()) {
    RTN_MSG(false, "Failed to init command buffer.\n");
  }

  is_initialized_ = true;

  return true;
}

bool VkLoopTest::InitBuffer() {
  const auto &device = ctx_->device();

  // Create buffer.
  constexpr size_t kBufferSize = 4096;
  vk::BufferCreateInfo buffer_info;
  buffer_info.size = kBufferSize;
  buffer_info.usage = vk::BufferUsageFlagBits::eStorageBuffer;
  buffer_info.sharingMode = vk::SharingMode::eExclusive;

  auto rvt_buffer = device->createBufferUnique(buffer_info);
  if (vk::Result::eSuccess != rvt_buffer.result) {
    RTN_MSG(false, "VK Error: 0x%x - Create buffer.\n", rvt_buffer.result);
  }
  buffer_ = std::move(rvt_buffer.value);

  // Find host visible buffer memory type.
  vk::PhysicalDeviceMemoryProperties memory_props;
  ctx_->physical_device().getMemoryProperties(&memory_props);

  uint32_t memory_type = 0;
  for (; memory_type < memory_props.memoryTypeCount; memory_type++) {
    if (memory_props.memoryTypes[memory_type].propertyFlags &
        vk::MemoryPropertyFlagBits::eHostVisible) {
      break;
    }
  }
  if (memory_type >= memory_props.memoryTypeCount) {
    RTN_MSG(false, "Can't find host visible memory for buffer.\n");
  }

  // Allocate buffer memory.
  vk::MemoryRequirements buffer_memory_reqs = device->getBufferMemoryRequirements(*buffer_);
  vk::MemoryAllocateInfo alloc_info;
  alloc_info.allocationSize = buffer_memory_reqs.size;
  alloc_info.memoryTypeIndex = memory_type;

  auto rvt_memory = device->allocateMemoryUnique(alloc_info);
  if (vk::Result::eSuccess != rvt_memory.result) {
    RTN_MSG(false, "VK Error: 0x%x - Create buffer memory.\n", rvt_memory.result);
  }
  buffer_memory_ = std::move(rvt_memory.value);

  // Map, set, flush and bind buffer memory.
  void *addr;
  auto rv_map =
      device->mapMemory(*buffer_memory_, 0 /* offset */, kBufferSize, vk::MemoryMapFlags(), &addr);
  if (vk::Result::eSuccess != rv_map) {
    RTN_MSG(false, "VK Error: 0x%x - Map buffer memory.\n", rv_map);
  }

  // Set to 1 so the shader will ping pong about zero.
  *reinterpret_cast<uint32_t *>(addr) = 1;

  vk::MappedMemoryRange memory_range;
  memory_range.memory = *buffer_memory_;
  memory_range.size = VK_WHOLE_SIZE;

  auto rv_flush = device->flushMappedMemoryRanges(1, &memory_range);
  if (vk::Result::eSuccess != rv_flush) {
    RTN_MSG(false, "VK Error: 0x%x - Flush buffer memory range.\n", rv_flush);
  }

  auto rv_bind = device->bindBufferMemory(*buffer_, *buffer_memory_, 0 /* offset */);
  if (vk::Result::eSuccess != rv_bind) {
    RTN_MSG(false, "VK Error: 0x%x - Bind buffer memory.\n", rv_bind);
  }

  return true;
}

bool VkLoopTest::InitCommandBuffer() {
  const auto &device = *ctx_->device();
  vk::CommandPoolCreateInfo command_pool_info;
  command_pool_info.queueFamilyIndex = ctx_->queue_family_index();

  auto rvt_command_pool = ctx_->device()->createCommandPoolUnique(command_pool_info);
  if (vk::Result::eSuccess != rvt_command_pool.result) {
    RTN_MSG(false, "VK Error: 0x%x - Create command pool.\n", rvt_command_pool.result);
  }
  command_pool_ = std::move(rvt_command_pool.value);

  vk::CommandBufferAllocateInfo cmd_buff_alloc_info;
  cmd_buff_alloc_info.commandPool = *command_pool_;
  cmd_buff_alloc_info.level = vk::CommandBufferLevel::ePrimary;
  cmd_buff_alloc_info.commandBufferCount = 1;

  auto rvt_alloc_cmd_bufs = ctx_->device()->allocateCommandBuffersUnique(cmd_buff_alloc_info);
  if (vk::Result::eSuccess != rvt_alloc_cmd_bufs.result) {
    RTN_MSG(false, "VK Error: 0x%x - Allocate command buffers.\n", rvt_alloc_cmd_bufs.result);
  }
  command_buffers_ = std::move(rvt_alloc_cmd_bufs.value);
  vk::UniqueCommandBuffer &command_buffer = command_buffers_.front();

  auto rv_begin = command_buffer->begin(vk::CommandBufferBeginInfo{});
  if (vk::Result::eSuccess != rv_begin) {
    RTN_MSG(false, "VK Error: 0x%x - Begin command buffer.\n", rv_begin);
  }

  VkShaderModule compute_shader_module_;
  VkShaderModuleCreateInfo sh_info = {};
  sh_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;

  std::vector<uint8_t> shader;
  {
    int fd = open("/pkg/data/vkloop.spv", O_RDONLY);
    if (fd < 0) {
      RTN_MSG(false, "Couldn't open shader binary: %d\n", fd);
    }

    struct stat buf;
    fstat(fd, &buf);
    shader.resize(buf.st_size);
    read(fd, shader.data(), shader.size());
    close(fd);

    sh_info.codeSize = shader.size();
    sh_info.pCode = reinterpret_cast<uint32_t *>(shader.data());
  }

  VkResult result;
  if ((result = vkCreateShaderModule(device, &sh_info, NULL, &compute_shader_module_)) !=
      VK_SUCCESS) {
    RTN_MSG(false, "vkCreateShaderModule failed: %d\n", result);
  }

  VkDescriptorSetLayoutBinding descriptor_set_layout_bindings = {
      .binding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      .pImmutableSamplers = nullptr};

  VkDescriptorSetLayoutCreateInfo descriptor_set_layout_create_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .pNext = nullptr,
      .bindingCount = 1,
      .pBindings = &descriptor_set_layout_bindings,
  };

  VkDescriptorSetLayout descriptor_set_layout;

  if ((result = vkCreateDescriptorSetLayout(device, &descriptor_set_layout_create_info, nullptr,
                                            &descriptor_set_layout)) != VK_SUCCESS) {
    RTN_MSG(false, "vkCreateDescriptorSetLayout failed: %d\n", result);
  }

  VkDescriptorPoolSize pool_size = {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                    .descriptorCount = 1};

  VkDescriptorPoolCreateInfo descriptor_pool_create_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .maxSets = 1,
      .poolSizeCount = 1,
      .pPoolSizes = &pool_size};

  VkDescriptorPool descriptor_pool;
  if ((result = vkCreateDescriptorPool(device, &descriptor_pool_create_info, nullptr,
                                       &descriptor_pool)) != VK_SUCCESS) {
    RTN_MSG(false, "vkCreateDescriptorPool failed: %d\n", result);
  }

  VkDescriptorSetAllocateInfo descriptor_set_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .pNext = nullptr,
      .descriptorPool = descriptor_pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &descriptor_set_layout,
  };

  if ((result = vkAllocateDescriptorSets(device, &descriptor_set_allocate_info,
                                         &vk_descriptor_set_)) != VK_SUCCESS) {
    RTN_MSG(false, "vkAllocateDescriptorSets failed: %d\n", result);
  }

  VkDescriptorBufferInfo descriptor_buffer_info = {
      .buffer = *buffer_, .offset = 0, .range = VK_WHOLE_SIZE};

  VkWriteDescriptorSet write_descriptor_set = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                               .pNext = nullptr,
                                               .dstSet = vk_descriptor_set_,
                                               .dstBinding = 0,
                                               .dstArrayElement = 0,
                                               .descriptorCount = 1,
                                               .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                               .pImageInfo = nullptr,
                                               .pBufferInfo = &descriptor_buffer_info,
                                               .pTexelBufferView = nullptr};
  vkUpdateDescriptorSets(device,
                         1,  // descriptorWriteCount
                         &write_descriptor_set,
                         0,         // descriptorCopyCount
                         nullptr);  // pDescriptorCopies

  VkPipelineLayoutCreateInfo pipeline_create_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .setLayoutCount = 1,
      .pSetLayouts = &descriptor_set_layout,
      .pushConstantRangeCount = 0,
      .pPushConstantRanges = nullptr};

  if ((result = vkCreatePipelineLayout(device, &pipeline_create_info, nullptr,
                                       &vk_pipeline_layout_)) != VK_SUCCESS) {
    RTN_MSG(false, "vkCreatePipelineLayout failed: %d\n", result);
  }

  VkComputePipelineCreateInfo pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = compute_shader_module_,
                .pName = "main",
                .pSpecializationInfo = nullptr},
      .layout = vk_pipeline_layout_,
      .basePipelineHandle = VK_NULL_HANDLE,
      .basePipelineIndex = 0};

  if ((result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                         &vk_compute_pipeline_)) != VK_SUCCESS) {
    RTN_MSG(false, "vkCreateComputePipelines failed: %d\n", result);
  }

  if (hang_on_event_) {
    VkEventCreateInfo event_info = {
        .sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO, .pNext = nullptr, .flags = 0};
    if ((result = vkCreateEvent(device, &event_info, nullptr, &vk_event_)) != VK_SUCCESS) {
      RTN_MSG(false, "vkCreateEvent failed: %d\n", result);
    }

    vkCmdWaitEvents(*command_buffer, 1, &vk_event_, VK_PIPELINE_STAGE_HOST_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0, nullptr, 0, nullptr, 0, nullptr);
  } else {
    vkCmdBindPipeline(*command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, vk_compute_pipeline_);

    vkCmdBindDescriptorSets(*command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, vk_pipeline_layout_,
                            0,  // firstSet
                            1,  // descriptorSetCount,
                            &vk_descriptor_set_,
                            0,         // dynamicOffsetCount
                            nullptr);  // pDynamicOffsets

    vkCmdDispatch(*command_buffer, 1, 1, 1);
  }

  auto rv_end = command_buffer->end();
  if (vk::Result::eSuccess != rv_end) {
    RTN_MSG(false, "VK Error: 0x%x - End command buffer.\n", rv_end);
  }

  return true;
}

bool VkLoopTest::Exec(bool kill_driver) {
  auto rv_wait = ctx_->queue().waitIdle();
  if (vk::Result::eSuccess != rv_wait) {
    RTN_MSG(false, "VK Error: 0x%x - Queue wait idle.\n", rv_wait);
  }

  // Submit command buffer and wait for it to complete.
  vk::SubmitInfo submit_info;
  submit_info.commandBufferCount = static_cast<uint32_t>(command_buffers_.size());
  const vk::CommandBuffer &command_buffer = command_buffers_.front().get();
  submit_info.pCommandBuffers = &command_buffer;

  auto rv = ctx_->queue().submit(1 /* submitCt */, &submit_info, nullptr /* fence */);
  if (rv != vk::Result::eSuccess) {
    RTN_MSG(false, "VK Error: 0x%x - vk::Queue submit failed.\n", rv);
  }

  if (kill_driver) {
    magma::TestDeviceBase test_device(ctx_->physical_device().getProperties().vendorID);
    uint64_t is_supported = 0;
    magma_status_t status =
        magma_query2(test_device.device(), MAGMA_QUERY_IS_TEST_RESTART_SUPPORTED, &is_supported);
    if (status != MAGMA_STATUS_OK || !is_supported) {
      RTN_MSG(true, "Test restart not supported: status %d is_supported %lu\n", status,
              is_supported);
    }

    // TODO: Unbind and rebind driver once that supports forcibly tearing down client connections.
    EXPECT_EQ(ZX_OK, fuchsia_gpu_magma_DeviceTestRestart(test_device.channel()->get()));
  }

  constexpr int kReps = 5;
  for (int i = 0; i < kReps; i++) {
    rv_wait = ctx_->queue().waitIdle();
    if (vk::Result::eSuccess != rv_wait) {
      break;
    }
  }
  if (vk::Result::eErrorDeviceLost != rv_wait) {
    RTN_MSG(false, "VK Error: Result was 0x%x instead of vk::Result::eErrorDeviceLost\n", rv_wait);
  }

  return true;
}

TEST(Vulkan, InfiniteLoop) {
  for (int i = 0; i < 2; i++) {
    VkLoopTest test(false);
    ASSERT_TRUE(test.Initialize());
    ASSERT_TRUE(test.Exec(false));
  }
}

TEST(Vulkan, EventHang) {
  VkLoopTest test(true);
  ASSERT_TRUE(test.Initialize());
  ASSERT_TRUE(test.Exec(false));
}

TEST(Vulkan, DriverDeath) {
  VkLoopTest test(true);
  ASSERT_TRUE(test.Initialize());
  ASSERT_TRUE(test.Exec(true));
}

}  // namespace
