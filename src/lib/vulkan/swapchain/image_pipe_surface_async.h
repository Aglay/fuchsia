// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LIB_VULKAN_SWAPCHAIN_IMAGE_PIPE_SURFACE_ASYNC_H_
#define SRC_LIB_VULKAN_SWAPCHAIN_IMAGE_PIPE_SURFACE_ASYNC_H_

#include <fuchsia/sysmem/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>

#include <mutex>
#include <thread>
#include <unordered_map>

#include "image_pipe_surface.h"

namespace image_pipe_swapchain {

// An implementation of ImagePipeSurface based on an async fidl ImagePipe.
class ImagePipeSurfaceAsync : public ImagePipeSurface {
 public:
  explicit ImagePipeSurfaceAsync(zx_handle_t image_pipe_handle)
      : loop_(&kAsyncLoopConfigNoAttachToCurrentThread) {
    image_pipe_.Bind(zx::channel(image_pipe_handle), loop_.dispatcher());
    loop_.StartThread();
  }

  bool Init() override;

  bool CreateImage(VkDevice device, VkLayerDispatchTable* pDisp, VkFormat format,
                   VkImageUsageFlags usage, VkSwapchainCreateFlagsKHR swapchain_flags,
                   fuchsia::images::ImageInfo image_info, uint32_t image_count,
                   const VkAllocationCallbacks* pAllocator,
                   std::vector<ImageInfo>* image_info_out) override;

  void RemoveImage(uint32_t image_id) override;

  void PresentImage(uint32_t image_id, std::vector<zx::event> acquire_fences,
                    std::vector<zx::event> release_fences) override;

 private:
  void PresentNextImageLocked();

  async::Loop loop_;
  std::mutex mutex_;
  fuchsia::images::ImagePipe2Ptr image_pipe_;

  uint32_t current_buffer_id_ = 0;
  std::unordered_map</*image_id=*/uint32_t, /*buffer_id=*/uint32_t> image_id_to_buffer_id_;
  std::unordered_map</*buffer_id=*/uint32_t, /*image count=*/uint32_t> buffer_counts_;

  struct PendingPresent {
    uint32_t image_id;
    std::vector<zx::event> acquire_fences;
    std::vector<zx::event> release_fences;
  };
  std::vector<PendingPresent> queue_;
  bool present_pending_ = false;
  fuchsia::sysmem::AllocatorSyncPtr sysmem_allocator_;
};

}  // namespace image_pipe_swapchain

#endif  // SRC_LIB_VULKAN_SWAPCHAIN_IMAGE_PIPE_SURFACE_ASYNC_H_
