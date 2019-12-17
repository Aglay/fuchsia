// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "magma_util/macros.h"
#include "platform_connection.h"
#include "platform_connection_client.h"

namespace magma {

class LinuxPlatformConnectionClient : public PlatformConnectionClient {
 public:
  LinuxPlatformConnectionClient(PlatformConnection::Delegate* delegate) : delegate_(delegate) {}

  magma_status_t ImportBuffer(PlatformBuffer* buffer) override;

  magma_status_t ReleaseBuffer(uint64_t buffer_id) override;

  magma_status_t ImportObject(uint32_t handle, PlatformObject::Type object_type) override;

  magma_status_t ReleaseObject(uint64_t object_id, PlatformObject::Type object_type) override;

  void CreateContext(uint32_t* context_id_out) override;

  void DestroyContext(uint32_t context_id) override;

  magma_status_t GetError() override;

  magma_status_t MapBufferGpu(uint64_t buffer_id, uint64_t gpu_va, uint64_t page_offset,
                              uint64_t page_count, uint64_t flags) override;

  magma_status_t UnmapBufferGpu(uint64_t buffer_id, uint64_t gpu_va) override;

  magma_status_t CommitBuffer(uint64_t buffer_id, uint64_t page_offset,
                              uint64_t page_count) override;

  uint32_t GetNotificationChannelHandle() override { return 0; }

  magma_status_t ReadNotificationChannel(void* buffer, size_t buffer_size,
                                         size_t* buffer_size_out) override;

  magma_status_t WaitNotificationChannel(int64_t timeout_ns) override;

  void ExecuteCommandBufferWithResources(uint32_t context_id,
                                         magma_system_command_buffer* command_buffer,
                                         magma_system_exec_resource* resources,
                                         uint64_t* semaphores) override;

  void ExecuteImmediateCommands(uint32_t context_id, uint64_t command_count,
                                magma_inline_command_buffer* command_buffers) override;

 private:
  PlatformConnection::Delegate* delegate_;
  uint32_t next_context_id_ = 0;
};

}  // namespace magma
