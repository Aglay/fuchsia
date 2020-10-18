// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_SCENIC_LIB_FLATLAND_RENDERER_VK_RENDERER_H_
#define SRC_UI_SCENIC_LIB_FLATLAND_RENDERER_VK_RENDERER_H_

#include <unordered_map>

#include "src/ui/lib/escher/flatland/rectangle_compositor.h"
#include "src/ui/scenic/lib/flatland/renderer/gpu_mem.h"
#include "src/ui/scenic/lib/flatland/renderer/renderer.h"

namespace flatland {

// Implementation of the Flatland Renderer interface that relies on Escher and
// by extension the Vulkan API.
class VkRenderer final : public Renderer {
 public:
  explicit VkRenderer(std::unique_ptr<escher::Escher> escher);
  ~VkRenderer() override;

  // |BufferCollectionImporter|
  bool ImportBufferCollection(
      sysmem_util::GlobalBufferCollectionId collection_id,
      fuchsia::sysmem::Allocator_Sync* sysmem_allocator,
      fidl::InterfaceHandle<fuchsia::sysmem::BufferCollectionToken> token) override;

  // |BufferCollectionImporter|
  void ReleaseBufferCollection(sysmem_util::GlobalBufferCollectionId collection_id) override;

  // |BufferCollectionImporter|
  bool ImportImage(const ImageMetadata& meta_data) override;

  // |BufferCollectionImporter|
  void ReleaseImage(GlobalImageId image_id) override;

  // |Renderer|.
  bool RegisterRenderTargetCollection(
      sysmem_util::GlobalBufferCollectionId collection_id,
      fuchsia::sysmem::Allocator_Sync* sysmem_allocator,
      fidl::InterfaceHandle<fuchsia::sysmem::BufferCollectionToken> token) override;

  // |Renderer|.
  void DeregisterRenderTargetCollection(
      sysmem_util::GlobalBufferCollectionId collection_id) override;

  // |Renderer|.
  std::optional<BufferCollectionMetadata> Validate(
      sysmem_util::GlobalBufferCollectionId collection_id) override;

  // |Renderer|.
  void Render(const ImageMetadata& render_target, const std::vector<Rectangle2D>& rectangles,
              const std::vector<ImageMetadata>& images,
              const std::vector<zx::event>& release_fences = {}) override;

  // Wait for all gpu operations to complete.
  void WaitIdle();

 private:
  // Generic helper function used by both |ImportBufferCollection| and |RegisterTextureCollection|.
  bool RegisterCollection(sysmem_util::GlobalBufferCollectionId collection_id,
                          fuchsia::sysmem::Allocator_Sync* sysmem_allocator,
                          fidl::InterfaceHandle<fuchsia::sysmem::BufferCollectionToken> token,
                          vk::ImageUsageFlags usage);

  // The function ExtractImage() creates an escher Image from a sysmem collection vmo.
  escher::ImagePtr ExtractImage(escher::CommandBuffer* command_buffer, ImageMetadata metadata,
                                vk::ImageUsageFlags usage, vk::ImageLayout layout);

  // ExtractRenderTarget() and ExtractTexture() are wrapper functions to ExtractImage()
  // that provide specific usage flags for color attachments and shader textures
  // respectively. All three functions are thread safe. Additionally, they only get
  // called from Render() and by extension the render thread.
  escher::TexturePtr ExtractTexture(escher::CommandBuffer* command_buffer, ImageMetadata metadata);
  escher::ImagePtr ExtractRenderTarget(escher::CommandBuffer* command_buffer,
                                       ImageMetadata metadata);

  // Escher is how we access Vulkan.
  std::unique_ptr<escher::Escher> escher_;

  // Vulkan rendering component.
  escher::RectangleCompositor compositor_;

  // This mutex is used to protect access to |collection_map_|, |collection_metadata_map_|,
  // and |vk_collection_map_|.
  std::mutex lock_;
  std::unordered_map<sysmem_util::GlobalBufferCollectionId, BufferCollectionInfo> collection_map_;
  std::unordered_map<sysmem_util::GlobalBufferCollectionId, BufferCollectionMetadata>
      collection_metadata_map_;
  std::unordered_map<sysmem_util::GlobalBufferCollectionId, vk::BufferCollectionFUCHSIA>
      vk_collection_map_;

  uint32_t frame_number_ = 0;
};

}  // namespace flatland

#endif  // SRC_UI_SCENIC_LIB_FLATLAND_RENDERER_VK_RENDERER_H_
