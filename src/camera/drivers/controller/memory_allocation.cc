// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "memory_allocation.h"

#include <lib/syslog/global.h>
#include <zircon/errors.h>

#include "fuchsia/sysmem/cpp/fidl.h"

namespace camera {

zx_status_t ControllerMemoryAllocator::AllocateSharedMemory(
    std::vector<fuchsia::sysmem::BufferCollectionConstraints> constraints,
    fuchsia::sysmem::BufferCollectionInfo_2* out_buffer_collection_info) {
  if (out_buffer_collection_info == nullptr) {
    return ZX_ERR_INVALID_ARGS;
  }

  auto num_constraints = constraints.size();

  // Create tokens which we'll hold on to to get our buffer_collection.
  std::vector<fuchsia::sysmem::BufferCollectionTokenSyncPtr> tokens(num_constraints);

  // Start the allocation process.
  auto status = sysmem_allocator_->AllocateSharedCollection(tokens[0].NewRequest());
  if (status != ZX_OK) {
    FX_LOGF(ERROR, "", "%s: Failed to create token \n", __func__);
    return status;
  }

  // Duplicate the tokens.
  for (uint32_t i = 1; i < num_constraints; i++) {
    status = tokens[0]->Duplicate(std::numeric_limits<uint32_t>::max(), tokens[i].NewRequest());
    if (status != ZX_OK) {
      FX_LOGF(ERROR, "", "%s: Failed to duplicate token \n", __func__);
      return status;
    }
  }

  // Now convert into a Logical BufferCollection:
  std::vector<fuchsia::sysmem::BufferCollectionSyncPtr> buffer_collections(num_constraints);

  status = sysmem_allocator_->BindSharedCollection(std::move(tokens[0]),
                                                   buffer_collections[0].NewRequest());
  if (status != ZX_OK) {
    FX_LOGF(ERROR, "", "%s: Failed to create logical buffer collection \n", __func__);
    return status;
  }

  status = buffer_collections[0]->Sync();
  if (status != ZX_OK) {
    FX_LOGF(ERROR, "", "%s: Failed to sync \n", __func__);
    return status;
  }

  // Create rest of the logical buffer collections
  for (uint32_t i = 1; i < num_constraints; i++) {
    status = sysmem_allocator_->BindSharedCollection(std::move(tokens[i]),
                                                     buffer_collections[i].NewRequest());
    if (status != ZX_OK) {
      FX_LOGF(ERROR, "", "%s: Failed to create logical buffer collection \n", __func__);
      return status;
    }
  }

  // Set constraints
  for (uint32_t i = 0; i < num_constraints; i++) {
    status = buffer_collections[i]->SetConstraints(true, constraints[i]);
    if (status != ZX_OK) {
      FX_LOGF(ERROR, "", "%s: Failed to set buffer collection constraints \n", __func__);
      return status;
    }
  }

  zx_status_t allocation_status;
  status = buffer_collections[0]->WaitForBuffersAllocated(&allocation_status,
                                                          out_buffer_collection_info);
  if (status != ZX_OK || allocation_status != ZX_OK) {
    FX_LOGF(ERROR, "", "%s: Failed to  wait for buffer collection info.\n", __func__);
    return status;
  }

  for (uint32_t i = 0; i < num_constraints; i++) {
    status = buffer_collections[i]->Close();
    if (status != ZX_OK) {
      FX_LOGF(ERROR, "", "%s: Failed to close producer buffer collection \n", __func__);
      return status;
    }
  }

  // TODO(38569): Keep at least one buffer collection around to know about
  // any failures sysmem wants to notify by closing the channel.
  return ZX_OK;
}

}  // namespace camera
