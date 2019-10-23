// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/scenic/lib/gfx/resources/image_pipe2.h"

#include <fuchsia/sysmem/cpp/fidl.h>
#include <lib/fdio/directory.h>

#include <trace/event.h>

#include "src/ui/lib/escher/flib/fence.h"
#include "src/ui/scenic/lib/gfx/engine/frame_scheduler.h"
#include "src/ui/scenic/lib/gfx/engine/image_pipe_updater.h"
#include "src/ui/scenic/lib/gfx/engine/session.h"
#include "src/ui/scenic/lib/gfx/resources/gpu_image.h"
#include "src/ui/scenic/lib/gfx/util/time.h"

namespace scenic_impl {
namespace gfx {

namespace {

vk::ImageCreateInfo GetDefaultImageConstraints() {
  vk::ImageCreateInfo create_info;
  create_info.imageType = vk::ImageType::e2D;
  create_info.extent = vk::Extent3D{1, 1, 1};
  create_info.flags = vk::ImageCreateFlagBits::eMutableFormat;
  create_info.format = vk::Format::eB8G8R8A8Unorm;
  create_info.mipLevels = 1;
  create_info.arrayLayers = 1;
  create_info.samples = vk::SampleCountFlagBits::e1;
  create_info.tiling = vk::ImageTiling::eOptimal;
  create_info.usage = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled;
  create_info.sharingMode = vk::SharingMode::eExclusive;
  create_info.initialLayout = vk::ImageLayout::eUndefined;
  return create_info;
}

}  // namespace

const ResourceTypeInfo ImagePipe2::kTypeInfo = {ResourceType::kImagePipe | ResourceType::kImageBase,
                                                "ImagePipe2"};

ImagePipe2::ImagePipe2(Session* session, ResourceId id,
                       ::fidl::InterfaceRequest<fuchsia::images::ImagePipe2> request,
                       std::shared_ptr<ImagePipeUpdater> image_pipe_updater,
                       std::shared_ptr<ErrorReporter> error_reporter)
    : ImagePipeBase(session, id, ImagePipe2::kTypeInfo),
      handler_(std::make_unique<ImagePipe2Handler>(std::move(request), this)),
      session_(session),
      image_pipe_updater_(std::move(image_pipe_updater)),
      error_reporter_(std::move(error_reporter)),
      weak_ptr_factory_(this) {
  FXL_CHECK(image_pipe_updater_);
  FXL_CHECK(error_reporter_);

  // TODO(35547): Use a common SysmemAllocator instance for all ImagePipes.
  // Connect to Sysmem in preparation for the future AddBufferCollection() calls.
  zx_status_t status = fdio_service_connect("/svc/fuchsia.sysmem.Allocator",
                                            sysmem_allocator_.NewRequest().TakeChannel().release());
  if (status != ZX_OK) {
    sysmem_allocator_.Unbind();
    error_reporter_->ERROR() << __func__ << ": Could not connect to sysmem";
  }
}

ImagePipe2::~ImagePipe2() { CloseConnectionAndCleanUp(); }

void ImagePipe2::AddBufferCollection(
    uint32_t buffer_collection_id,
    ::fidl::InterfaceHandle<::fuchsia::sysmem::BufferCollectionToken> buffer_collection_token) {
  TRACE_DURATION("gfx", "ImagePipe2::AddBufferCollection", "buffer_collection_id",
                 buffer_collection_id);

  if (buffer_collection_id == 0) {
    error_reporter_->ERROR() << __func__ << ": BufferCollection can not be assigned an ID of 0.";
    CloseConnectionAndCleanUp();
    return;
  }

  if (buffer_collections_.find(buffer_collection_id) != buffer_collections_.end()) {
    error_reporter_->ERROR() << __func__ << ": resource with ID " << buffer_collection_id
                             << " already exists.";
    CloseConnectionAndCleanUp();
    return;
  }

  if (!buffer_collection_token.is_valid()) {
    error_reporter_->ERROR() << __func__ << ": Token is invalid.";
    CloseConnectionAndCleanUp();
    return;
  }

  // Duplicate token for vulkan to set constraints.
  fuchsia::sysmem::BufferCollectionTokenSyncPtr local_token = buffer_collection_token.BindSync();
  if (!local_token) {
    error_reporter_->ERROR() << __func__ << ": could not bind Token.";
    CloseConnectionAndCleanUp();
    return;
  }
  fuchsia::sysmem::BufferCollectionTokenSyncPtr vulkan_token;
  zx_status_t status =
      local_token->Duplicate(std::numeric_limits<uint32_t>::max(), vulkan_token.NewRequest());
  if (status != ZX_OK) {
    error_reporter_->ERROR() << __func__ << ": Token Duplicate failed: " << status;
    CloseConnectionAndCleanUp();
    return;
  }
  status = local_token->Sync();
  if (status != ZX_OK) {
    error_reporter_->ERROR() << __func__ << ": Token Sync failed: " << status;
    CloseConnectionAndCleanUp();
    return;
  }

  // Use local token to create a BufferCollection. This will be saved for later checks in
  // AddImage().
  fuchsia::sysmem::BufferCollectionSyncPtr buffer_collection;
  status = sysmem_allocator_->BindSharedCollection(std::move(local_token),
                                                   buffer_collection.NewRequest());
  if (status != ZX_OK) {
    error_reporter_->ERROR() << __func__ << ": BindSharedCollection failed: " << status;
    CloseConnectionAndCleanUp();
    return;
  }

  // Set dummy constraints for |local_token| because all tokens needs to be used before allocation.
  // Also, |has_constraints| should be true to acess VMOs.
  fuchsia::sysmem::BufferCollectionConstraints constraints;
  constraints.min_buffer_count = 1;
  // Used because every constraints need to have a usage.
  constraints.usage.vulkan = fuchsia::sysmem::vulkanUsageSampled;
  status = buffer_collection->SetConstraints(true /* has_constraints */, constraints);
  if (status != ZX_OK) {
    error_reporter_->ERROR() << __func__ << ": SetConstraints failed:" << status;
    CloseConnectionAndCleanUp();
    return;
  }

  // Set VkImage constraints
  const vk::ImageCreateInfo& create_info = GetDefaultImageConstraints();
  vk::BufferCollectionFUCHSIA buffer_collection_fuchsia;
  const bool set_constraints = SetBufferCollectionConstraints(
      session_, std::move(vulkan_token), create_info, &buffer_collection_fuchsia);
  if (!set_constraints) {
    error_reporter_->ERROR() << __func__ << ": SetConstraints failed:" << status;
    CloseConnectionAndCleanUp();
    return;
  }

  buffer_collections_[buffer_collection_id] = {std::move(buffer_collection),
                                               buffer_collection_fuchsia};
}

void ImagePipe2::AddImage(uint32_t image_id, uint32_t buffer_collection_id,
                          uint32_t buffer_collection_index,
                          ::fuchsia::sysmem::ImageFormat_2 image_format) {
  TRACE_DURATION("gfx", "ImagePipe2::AddImage", "image_id", image_id);

  if (image_id == 0) {
    error_reporter_->ERROR() << __func__ << ": Image can not be assigned an ID of 0.";
    CloseConnectionAndCleanUp();
    return;
  }

  if (images_.find(image_id) != images_.end()) {
    error_reporter_->ERROR() << __func__ << ": image with ID " << buffer_collection_id
                             << " already exists.";
    CloseConnectionAndCleanUp();
    return;
  }

  auto buffer_collection_it = buffer_collections_.find(buffer_collection_id);
  if (buffer_collection_it == buffer_collections_.end()) {
    error_reporter_->ERROR() << __func__ << ": resource with ID not found.";
    CloseConnectionAndCleanUp();
    return;
  }

  // Wait for the buffers to be allocated before adding the first Image.
  BufferCollectionInfo& info = buffer_collection_it->second;
  if (!info.buffer_collection_info.buffer_count) {
    zx_status_t allocation_status = ZX_OK;
    zx_status_t status = info.buffer_collection_ptr->CheckBuffersAllocated(&allocation_status);
    if (status != ZX_OK || allocation_status != ZX_OK) {
      error_reporter_->ERROR() << __func__ << ": CheckBuffersAllocated failed" << status << " "
                               << allocation_status;
      CloseConnectionAndCleanUp();
      return;
    }
    status = info.buffer_collection_ptr->WaitForBuffersAllocated(&allocation_status,
                                                                 &info.buffer_collection_info);
    if (status != ZX_OK || allocation_status != ZX_OK) {
      error_reporter_->ERROR() << __func__ << ": WaitForBuffersAllocated failed" << status << " "
                               << allocation_status;
      CloseConnectionAndCleanUp();
      return;
    }
    FXL_DCHECK(info.buffer_collection_info.buffer_count > 0);
  }

  // Check given |buffer_collection_index| against actually allocated number of buffers.
  if (info.buffer_collection_info.buffer_count <= buffer_collection_index) {
    error_reporter_->ERROR() << __func__ << ": buffer_collection_index out of bounds";
    CloseConnectionAndCleanUp();
    return;
  }

  ImagePtr image = CreateImage(session_, image_id, info, buffer_collection_index, image_format);
  if (!image) {
    error_reporter_->ERROR() << __func__ << ": Unable to create gpu image.";
    CloseConnectionAndCleanUp();
    return;
  }

  FXL_DCHECK(info.images.find(image_id) == info.images.end());
  if (image->use_protected_memory()) {
    num_protected_images_++;
  }
  info.images.insert(image_id);
  images_.insert({image_id, std::move(image)});
}

void ImagePipe2::RemoveBufferCollection(uint32_t buffer_collection_id) {
  TRACE_DURATION("gfx", "ImagePipe2::RemoveBufferCollection", "buffer_collection_id",
                 buffer_collection_id);

  auto buffer_collection_it = buffer_collections_.find(buffer_collection_id);
  if (buffer_collection_it == buffer_collections_.end()) {
    error_reporter_->ERROR() << __func__ << ": resource with ID not found.";
    CloseConnectionAndCleanUp();
    return;
  }

  BufferCollectionInfo& info = buffer_collection_it->second;
  while (!info.images.empty()) {
    RemoveImage(*info.images.begin());
  }
  DestroyBufferCollection(session_, info.vk_buffer_collection);
  info.buffer_collection_ptr->Close();

  buffer_collections_.erase(buffer_collection_it);
}

void ImagePipe2::RemoveImage(uint32_t image_id) {
  TRACE_DURATION("gfx", "ImagePipe2::RemoveImage", "image_id", image_id);

  auto image_it = images_.find(image_id);
  if (image_it == images_.end()) {
    error_reporter_->ERROR() << __func__ << ": Could not find image with id=" << image_id << ".";
  }

  if (image_it->second->use_protected_memory()) {
    FXL_DCHECK(num_protected_images_ >= 1);
    num_protected_images_--;
  }

  images_.erase(image_it);

  for (auto& buffer_collection : buffer_collections_) {
    if (buffer_collection.second.images.erase(image_id)) {
      break;
    }
  }
}

void ImagePipe2::PresentImage(uint32_t image_id, zx::time presentation_time,
                              std::vector<::zx::event> acquire_fences,
                              std::vector<::zx::event> release_fences,
                              PresentImageCallback callback) {
  // NOTE: This name is important for benchmarking.  Do not remove or modify it
  // without also updating the script.
  TRACE_DURATION("gfx", "ImagePipe2::PresentImage", "image_id", image_id, "use_protected_memory",
                 use_protected_memory());
  TRACE_FLOW_END("gfx", "image_pipe_present_image", image_id);

  if (!frames_.empty() && presentation_time < frames_.back().presentation_time) {
    error_reporter_->ERROR()
        << __func__ << ": Present called with out-of-order presentation time. presentation_time="
        << presentation_time
        << ", last scheduled presentation time=" << frames_.back().presentation_time;
    CloseConnectionAndCleanUp();
    return;
  }

  // Verify that image_id is valid.
  auto image_it = images_.find(image_id);
  if (image_it == images_.end()) {
    error_reporter_->ERROR() << __func__ << ": could not find Image with ID: " << image_id;
    CloseConnectionAndCleanUp();
    return;
  }

  auto acquire_fences_listener =
      std::make_unique<escher::FenceSetListener>(std::move(acquire_fences));
  acquire_fences_listener->WaitReadyAsync(
      [weak = weak_ptr_factory_.GetWeakPtr(), presentation_time] {
        if (weak) {
          weak->image_pipe_updater_->ScheduleImagePipeUpdate(presentation_time, weak);
        }
      });
  TRACE_FLOW_BEGIN("gfx", "image_pipe_present_image_to_update", image_id);
  frames_.push(Frame{image_it->second, presentation_time, std::move(acquire_fences_listener),
                     fidl::VectorPtr(std::move(release_fences)), std::move(callback)});
}

ImagePipeUpdateResults ImagePipe2::Update(escher::ReleaseFenceSignaller* release_fence_signaller,
                                          zx::time presentation_time) {
  FXL_DCHECK(release_fence_signaller);

  ImagePipeUpdateResults results{.image_updated = false};

  bool present_next_image = false;
  ResourceId next_image_id = current_image_id_;
  std::vector<zx::event> next_release_fences;

  ImagePtr next_image = nullptr;
  while (!frames_.empty() && frames_.front().presentation_time <= presentation_time &&
         frames_.front().acquire_fences->ready()) {
    if (next_image) {
      // We're skipping a frame, so we should also mark the image as dirty, in
      // case the producer updates the pixels in the buffer between now and a
      // future present call.
      next_image->MarkAsDirty();
    }

    next_image = frames_.front().image;
    FXL_DCHECK(next_image);
    next_image_id = next_image->id();

    if (!next_release_fences.empty()) {
      // We're skipping a frame, so we can immediately signal its release
      // fences.
      for (auto& fence : next_release_fences) {
        fence.signal(0u, escher::kFenceSignalled);
      }
    }

    if (frames_.front().release_fences.has_value()) {
      next_release_fences = std::move(frames_.front().release_fences.value());
    } else {
      next_release_fences.clear();
    }

    results.callbacks.push(std::move(frames_.front().present_image_callback));
    TRACE_FLOW_END("gfx", "image_pipe_present_image_to_update", next_image_id);
    frames_.pop();
    present_next_image = true;
  }

  if (!present_next_image) {
    results.image_updated = false;
    return results;
  }

  // TODO(SCN-151): This code, and the code below that marks an image as dirty,
  // assumes that the same image cannot be presented twice in a row on the same
  // image pipe, while also requiring a call to UpdatePixels(). If not, this
  // needs a new test.
  if (next_image_id == current_image_id_) {
    // This ImagePipe did not change since the last frame was rendered.
    results.image_updated = false;
    return results;
  }

  // We're replacing a frame with a new one, so we hand off its release
  // fence to the |ReleaseFenceSignaller|, which will signal it as soon as
  // all work previously submitted to the GPU is finished.
  if (current_release_fences_) {
    release_fence_signaller->AddCPUReleaseFences(std::move(current_release_fences_));
  }
  current_release_fences_ = std::move(next_release_fences);
  current_image_id_ = next_image_id;
  // TODO(SCN-1010): Determine proper signaling for marking images as dirty.
  // For now, mark all released images as dirty, with the assumption that the
  // client will likely write into the buffer before submitting it again.
  if (current_image_) {
    current_image_->MarkAsDirty();
  }
  current_image_ = std::move(next_image);

  results.image_updated = true;
  return results;
}

void ImagePipe2::UpdateEscherImage(escher::BatchGpuUploader* gpu_uploader) {
  if (current_image_) {
    current_image_->UpdateEscherImage(gpu_uploader);
  }
}

const escher::ImagePtr& ImagePipe2::GetEscherImage() {
  if (current_image_) {
    return current_image_->GetEscherImage();
  }
  static const escher::ImagePtr kNullEscherImage;
  return kNullEscherImage;
}

bool ImagePipe2::SetBufferCollectionConstraints(
    Session* session, fuchsia::sysmem::BufferCollectionTokenSyncPtr token,
    const vk::ImageCreateInfo& create_info,
    vk::BufferCollectionFUCHSIA* out_buffer_collection_fuchsia) {
  // Set VkImage constraints using |create_info| on |token|
  auto vk_device = session->resource_context().vk_device;
  FXL_DCHECK(vk_device);
  auto vk_loader = session->resource_context().vk_loader;

  vk::BufferCollectionCreateInfoFUCHSIA buffer_collection_create_info;
  buffer_collection_create_info.collectionToken = token.Unbind().TakeChannel().release();
  auto create_buffer_collection_result =
      vk_device.createBufferCollectionFUCHSIA(buffer_collection_create_info, nullptr, vk_loader);
  if (create_buffer_collection_result.result != vk::Result::eSuccess) {
    error_reporter_->ERROR() << __func__ << ": VkCreateBufferCollectionFUCHSIA failed: "
                             << vk::to_string(create_buffer_collection_result.result);
    return false;
  }

  auto constraints_result = vk_device.setBufferCollectionConstraintsFUCHSIA(
      create_buffer_collection_result.value, create_info, vk_loader);
  if (constraints_result != vk::Result::eSuccess) {
    error_reporter_->ERROR() << __func__ << ": VkSetBufferCollectionConstraints failed: "
                             << vk::to_string(constraints_result);
    return false;
  }

  *out_buffer_collection_fuchsia = create_buffer_collection_result.value;
  return true;
}

void ImagePipe2::DestroyBufferCollection(Session* session,
                                         const vk::BufferCollectionFUCHSIA& vk_buffer_collection) {
  auto vk_device = session->resource_context().vk_device;
  FXL_DCHECK(vk_device);
  auto vk_loader = session->resource_context().vk_loader;
  vk_device.destroyBufferCollectionFUCHSIA(vk_buffer_collection, nullptr, vk_loader);
}

ImagePtr ImagePipe2::CreateImage(Session* session, ResourceId image_id,
                                 const ImagePipe2::BufferCollectionInfo& info,
                                 uint32_t buffer_collection_index,
                                 const ::fuchsia::sysmem::ImageFormat_2& image_format) {
  // Create Memory object pointing to the given |buffer_collection_index|.
  zx::vmo vmo;
  zx_status_t status = info.buffer_collection_info.buffers[buffer_collection_index].vmo.duplicate(
      ZX_RIGHT_SAME_RIGHTS, &vmo);
  if (status != ZX_OK) {
    error_reporter_->ERROR() << __func__ << ": vmo duplicate failed (err=" << status << ").";
    return nullptr;
  }

  auto vk_device = session->resource_context().vk_device;
  FXL_DCHECK(vk_device);
  auto vk_loader = session->resource_context().vk_loader;
  auto collection_properties =
      vk_device.getBufferCollectionPropertiesFUCHSIA(info.vk_buffer_collection, vk_loader);
  if (collection_properties.result != vk::Result::eSuccess) {
    error_reporter_->ERROR() << __func__
                             << ": VkGetBufferCollectionProperties failed (err=" << status << ").";
    return nullptr;
  }

  const uint32_t memory_type_index =
      escher::CountTrailingZeros(collection_properties.value.memoryTypeBits);
  vk::ImportMemoryBufferCollectionFUCHSIA import_info;
  import_info.collection = info.vk_buffer_collection;
  import_info.index = buffer_collection_index;
  vk::MemoryAllocateInfo alloc_info;
  alloc_info.setPNext(&import_info);
  alloc_info.memoryTypeIndex = memory_type_index;
  MemoryPtr memory = Memory::New(session, 0u, std::move(vmo), alloc_info, error_reporter_.get());
  if (!memory) {
    error_reporter_->ERROR() << __func__ << ": Unable to create a memory object.";
    return nullptr;
  }

  // Make a copy of |vk_image_create_info|. Set size constraint that we didn't have in
  // AddBufferCollection(). Also, check if |protected buffer| is allocated.
  vk::BufferCollectionImageCreateInfoFUCHSIA collection_image_info;
  collection_image_info.collection = info.vk_buffer_collection;
  collection_image_info.index = buffer_collection_index;
  vk::ImageCreateInfo image_create_info = GetDefaultImageConstraints();
  image_create_info.setPNext(&collection_image_info);
  image_create_info.extent = vk::Extent3D{image_format.coded_width, image_format.coded_height, 1};
  if (info.buffer_collection_info.settings.buffer_settings.is_secure) {
    image_create_info.flags = vk::ImageCreateFlagBits::eProtected;
  }

  // Create GpuImage object since Vulkan constraints set on BufferCollection guarantee that it will
  // be device memory.
  return GpuImage::New(session, image_id, memory, image_create_info, error_reporter_.get());
}

void ImagePipe2::CloseConnectionAndCleanUp() {
  handler_.reset();
  frames_ = {};
  while (!buffer_collections_.empty()) {
    RemoveBufferCollection(buffer_collections_.begin()->first);
  }
  buffer_collections_.clear();

  // Schedule a new frame.
  image_pipe_updater_->ScheduleImagePipeUpdate(zx::time(0), fxl::WeakPtr<ImagePipeBase>());
}

void ImagePipe2::OnConnectionError() { CloseConnectionAndCleanUp(); }

}  // namespace gfx
}  // namespace scenic_impl
