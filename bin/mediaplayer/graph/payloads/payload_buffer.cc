// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/mediaplayer/graph/payloads/payload_buffer.h"

#include <lib/zx/vmar.h>
#include <cstdlib>
#include <memory>
#include "lib/fxl/logging.h"

namespace media_player {

// static
fbl::RefPtr<PayloadVmo> PayloadVmo::Create(uint64_t vmo_size,
                                           const zx::handle* bti_handle) {
  FXL_DCHECK(vmo_size != 0);
  zx_status_t status;
  auto result = fbl::MakeRefCounted<PayloadVmo>(vmo_size, bti_handle, &status);
  return status == ZX_OK ? result : nullptr;
}

PayloadVmo::PayloadVmo(uint64_t vmo_size, const zx::handle* bti_handle,
                       zx_status_t* status_out)
    : size_(vmo_size) {
  FXL_DCHECK(vmo_size != 0);
  FXL_DCHECK(status_out != nullptr);

  zx_status_t status;

  if (bti_handle != nullptr) {
    // Create a contiguous VMO. This is a hack that will be removed once the
    // FIDL buffer allocator is working an integrated.
    status = zx_vmo_create_contiguous(bti_handle->get(), size_, 0,
                                      vmo_.reset_and_get_address());
    if (status != ZX_OK) {
      FXL_LOG(ERROR) << "Failed to create contiguous VMO of size " << size_
                     << ", status " << status << ".";
      *status_out = status;
      return;
    }
  } else {
    status = zx::vmo::create(size_, 0, &vmo_);
    if (status != ZX_OK) {
      FXL_LOG(ERROR) << "Failed to create VMO of size " << size_ << ", status "
                     << status << ".";
      *status_out = status;
      return;
    }
  }

  status = vmo_mapper_.Map(vmo_, 0, size_, ZX_VM_PERM_READ | ZX_VM_PERM_WRITE,
                           nullptr);
  if (status != ZX_OK) {
    FXL_LOG(ERROR) << "Failed to map VMO, status " << status;
    *status_out = status;
    return;
  }

  *status_out = ZX_OK;
}

zx::vmo PayloadVmo::Duplicate(zx_rights_t rights) {
  zx::vmo duplicate;
  zx_status_t status = vmo_.duplicate(rights, &duplicate);
  if (status != ZX_OK) {
    FXL_LOG(FATAL) << "Failed to duplicate VMO, status " << status;
  }

  return duplicate;
}

// static
fbl::RefPtr<PayloadBuffer> PayloadBuffer::Create(uint64_t size, void* data,
                                                 Recycler recycler) {
  return fbl::AdoptRef(new PayloadBuffer(size, data, std::move(recycler)));
}

// static
fbl::RefPtr<PayloadBuffer> PayloadBuffer::Create(uint64_t size, void* data,
                                                 fbl::RefPtr<PayloadVmo> vmo,
                                                 uint64_t offset,
                                                 Recycler recycler) {
  return fbl::AdoptRef(
      new PayloadBuffer(size, data, vmo, offset, std::move(recycler)));
}

// static
fbl::RefPtr<PayloadBuffer> PayloadBuffer::CreateWithMalloc(uint64_t size) {
  FXL_DCHECK(size > 0);
  // TODO: Once we use C++17, std::aligned_alloc should work.
  // |aligned_alloc| requires the size to the aligned.
  return PayloadBuffer::Create(size,
                               aligned_alloc(PayloadBuffer::kByteAlignment,
                                             PayloadBuffer::AlignUp(size)),
                               [](PayloadBuffer* payload_buffer) {
                                 FXL_DCHECK(payload_buffer);
                                 std::free(payload_buffer->data());
                                 // The |PayloadBuffer| deletes itself.
                               });
}

PayloadBuffer::PayloadBuffer(uint64_t size, void* data, Recycler recycler)
    : size_(size), data_(data), recycler_(std::move(recycler)) {
  FXL_DCHECK(size_ != 0);
  FXL_DCHECK(data_ != nullptr);
  FXL_DCHECK(recycler_);
}

PayloadBuffer::PayloadBuffer(uint64_t size, void* data,
                             fbl::RefPtr<PayloadVmo> vmo,
                             uint64_t offset_in_vmo, Recycler recycler)
    : size_(size),
      data_(data),
      vmo_(vmo),
      offset_(offset_in_vmo),
      recycler_(std::move(recycler)) {
  FXL_DCHECK(size_ != 0);
  FXL_DCHECK(vmo_);
  FXL_DCHECK((data_ == nullptr) ||
             (reinterpret_cast<uint8_t*>(vmo_->start()) + offset_ ==
              reinterpret_cast<uint8_t*>(data_)));
  FXL_DCHECK(recycler_);

  // TODO(dalesat): Remove this check when we support unmappable VMOs.
  FXL_DCHECK(data_ != nullptr);
}

PayloadBuffer::~PayloadBuffer() {
  FXL_DCHECK(!recycler_) << "PayloadBuffers must delete themselves.";
}

void PayloadBuffer::AfterRecycling(Action action) {
  FXL_DCHECK(!after_recycling_) << "AfterRecycling may only be called once.";
  after_recycling_ = std::move(action);
}

void PayloadBuffer::fbl_recycle() {
  FXL_DCHECK(recycler_ != nullptr);

  recycler_(this);
  // This tells the destructor that deletion is being done properly.
  recycler_ = nullptr;

  if (after_recycling_) {
    after_recycling_(this);
  }

  delete this;
}

}  // namespace media_player
