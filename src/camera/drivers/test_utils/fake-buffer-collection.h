// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CAMERA_DRIVERS_TEST_UTILS_FAKE_BUFFER_COLLECTION_H_
#define SRC_CAMERA_DRIVERS_TEST_UTILS_FAKE_BUFFER_COLLECTION_H_

#include <fuchsia/sysmem/c/fidl.h>
#include <lib/zx/vmo.h>
#include <zircon/device/sysmem.h>

namespace camera {

// Creates a BufferCollectionInfo that is allocated with contiguous memory
// This is meant as a helper utility for testing puroposes.
//    The format of the BufferCollection is hardcoded to NV12
//    There are no format modifiers, and the planes are unpopulated.
// |buffer_collection| : output BufferCollectionInfo
// |bti_handle|  bti used for allocating the contiguous vmos
// |width| width of the images in the buffer collection
// |height| height of the images in the buffer collection
// |num_buffers| number of buffers to allocate in the buffer collection
// @Return: ZX_OK if allocation works, otherwise returns status from
//          failed zx_vmo_create_contiguous.
zx_status_t CreateContiguousBufferCollectionInfo(
    fuchsia_sysmem_BufferCollectionInfo* buffer_collection, zx_handle_t bti_handle, uint32_t width,
    uint32_t height, uint32_t num_buffers);

// Create a fake ImageFormat2.
zx_status_t GetImageFormat2(uint32_t pixel_format_type, image_format_2_t& image_format,
                            uint32_t width, uint32_t height);

// Creates a BufferCollectionInfo2 that is allocated with contiguous memory, similar
// to the above.
zx_status_t CreateContiguousBufferCollectionInfo2(
    fuchsia_sysmem_BufferCollectionInfo_2& buffer_collection, const image_format_2_t& image_format,
    zx_handle_t bti_handle, uint32_t width, uint32_t height, uint32_t num_buffers);

}  // namespace camera

#endif  // SRC_CAMERA_DRIVERS_TEST_UTILS_FAKE_BUFFER_COLLECTION_H_
