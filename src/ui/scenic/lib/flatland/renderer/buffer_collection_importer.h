// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_SCENIC_LIB_FLATLAND_RENDERER_BUFFER_COLLECTION_IMPORTER_H_
#define SRC_UI_SCENIC_LIB_FLATLAND_RENDERER_BUFFER_COLLECTION_IMPORTER_H_

#include <fuchsia/sysmem/cpp/fidl.h>

#include "src/ui/scenic/lib/sysmem/id.h"

namespace flatland {

// Used to reference a particular flatland image.
using GlobalImageId = uint64_t;

// Struct representing the data needed to extract an image from a buffer collection.
// All pixel information is stored within the Vmo of the collection so this struct
// only needs information regarding which collection and which vmo to point to, and
// the overall size of the image. Only supports fuchsia::sysmem::PixelFormatType::BGRA32
// as the image format type.
struct ImageMetadata {
  // The unique id of the buffer collection this image is backed by.
  sysmem_util::GlobalBufferCollectionId collection_id;

  // The unique ID for this particular image.
  GlobalImageId identifier;

  // A single buffer collection may have several vmos. This tells the importer
  // which vmo in the collection specified by |collection_id| to use as the memory
  // for this image. This value must be less than BufferCollectionMetadata::vmo_count.
  uint32_t vmo_idx;

  // The dimensions of the image in pixels.
  uint32_t width, height;

  bool operator==(const ImageMetadata& meta) const {
    return collection_id == meta.collection_id && vmo_idx == meta.vmo_idx && width == meta.width &&
           height == meta.height;
  }
};

// This interface is used for importing Flatland buffer collections
// and images to external services that would like to also have access
// to the collection and set their own constraints. This interface allows
// Flatland to remain agnostic as to the implementation details of a
// particular service.
class BufferCollectionImporter {
 public:
  // Allows the service to set its own constraints on the buffer collection. Must be set before
  // the buffer collection is fully allocated/validated. The return value indicates successful
  // importation via |true| and a failed importation via |false|. The collection_id can be
  // reused if the importation fails.
  virtual bool ImportBufferCollection(
      sysmem_util::GlobalBufferCollectionId collection_id,
      fuchsia::sysmem::Allocator_Sync* sysmem_allocator,
      fidl::InterfaceHandle<fuchsia::sysmem::BufferCollectionToken> token) = 0;

  // Deregisters the buffer collection from the service. All images associated with the buffer
  // collection referenced by |collection_id| should be released via calls to |ReleaseImage|
  // before the buffer collection itself is released.
  virtual void ReleaseBufferCollection(sysmem_util::GlobalBufferCollectionId collection_id) = 0;

  // Has the service create an image for itself from the provided buffer collection. Returns
  // true upon a successful import and false otherwise.
  //
  // TODO(62240): Give more detailed errors.
  virtual bool ImportImage(const ImageMetadata& meta_data) = 0;

  // Deregisters the provided image from the service.
  virtual void ReleaseImage(GlobalImageId image_id) = 0;

  virtual ~BufferCollectionImporter() = default;
};

}  // namespace flatland

#endif  // SRC_UI_SCENIC_LIB_FLATLAND_RENDERER_BUFFER_COLLECTION_IMPORTER_H_
