// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "image_pipe_surface_async.h"

namespace image_pipe_swapchain {

void ImagePipeSurfaceAsync::AddImage(uint32_t image_id,
                                     fuchsia::images::ImageInfo image_info,
                                     zx::vmo buffer, uint64_t size_bytes) {
  std::lock_guard<std::mutex> lock(mutex_);
  image_pipe_->AddImage(image_id, std::move(image_info), std::move(buffer), 0,
                        size_bytes,
                        fuchsia::images::MemoryType::VK_DEVICE_MEMORY);
}

void ImagePipeSurfaceAsync::RemoveImage(uint32_t image_id) {
  std::unique_lock<std::mutex> lock(mutex_);
  for (auto iter = queue_.begin(); iter != queue_.end();) {
    if (iter->image_id == image_id) {
      iter = queue_.erase(iter);
    } else {
      iter++;
    }
  }
  // TODO(SCN-1107) - remove this workaround
  static constexpr bool kUseWorkaround = true;
  while (kUseWorkaround && present_pending_) {
    lock.unlock();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    lock.lock();
  }
  image_pipe_->RemoveImage(image_id);
}

void ImagePipeSurfaceAsync::PresentImage(
    uint32_t image_id, fidl::VectorPtr<zx::event> acquire_fences,
    fidl::VectorPtr<zx::event> release_fences) {
  std::lock_guard<std::mutex> lock(mutex_);
  queue_.push_back(
      {image_id, std::move(acquire_fences), std::move(release_fences)});
  if (!present_pending_) {
    PresentNextImageLocked();
  }
}

void ImagePipeSurfaceAsync::PresentNextImageLocked() {
  assert(!present_pending_);

  if (queue_.empty())
    return;

  // To guarantee FIFO mode, we can't have Scenic drop any of our frames.
  // We accomplish that sending the next one only when we receive the callback
  // for the previous one.  We don't use the presentation info timing
  // parameters because we really just want to push out the next image asap.
  uint64_t presentation_time = zx_clock_get_monotonic();

  auto& present = queue_.front();
  image_pipe_->PresentImage(present.image_id, presentation_time,
                            std::move(present.acquire_fences),
                            std::move(present.release_fences),
                            // This callback happening in a separate thread.
                            [this](fuchsia::images::PresentationInfo pinfo) {
                              std::lock_guard<std::mutex> lock(mutex_);
                              present_pending_ = false;
                              PresentNextImageLocked();
                            });

  queue_.erase(queue_.begin());
  present_pending_ = true;
}

}  // namespace image_pipe_swapchain
