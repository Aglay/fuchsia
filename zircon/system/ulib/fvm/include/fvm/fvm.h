// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FVM_FVM_H_
#define FVM_FVM_H_

#include <zircon/types.h>

#include <fvm/format.h>

namespace fvm {

// Helper class for obtaining information about the format of a FVM, such as superblock offsets,
// metadata size, allocated sizes, etc. It wraps a superblock (the "Header" structure) while
// keeping the Header a plain struct to ensure it stays PoD.
//
// This class is copyable assignable, and moveable.
class FormatInfo {
 public:
  FormatInfo() = default;
  FormatInfo(const Header& header) : header_(header) {}

  // Assumes a superblock created from the given disk for the given disk size, with slice size. (No
  // Preallocated metadata headers for future growth).
  static FormatInfo FromDiskSize(size_t disk_size, size_t slice_size);

  // Without instantiating a SuperBlock, assumes that an fvm will be formatted initially with
  // |initial_size| and eventually will grow up to |max_size| with |slice_size|.
  static FormatInfo FromPreallocatedSize(size_t initial_size, size_t max_size, size_t slice_size);

  // Access to the underlying header block. The hash of this structure may not be up-to-date.
  const Header& header() const { return header_; }

  // Returns the size of the addressable metadata in a FVM header.
  size_t metadata_size() const;

  // Returns the size of the allocated metadata SuperBlock. This may be bigger than
  // metadata_size() if there was extra space pre allocated for allowing fvm growing.
  size_t metadata_allocated_size() const;

  // Returns the number of addressable slices for the superblock, this is the number of physical
  // slices.
  size_t slice_count() const;

  // Returns the size of each slice in the described block.
  size_t slice_size() const { return header_.slice_size; }

  // Returns the offset of the given superblock. The first superblock is considered primary, in
  // terms of position.
  size_t GetSuperblockOffset(SuperblockType type) const {
    return (type == SuperblockType::kPrimary) ? 0 : metadata_allocated_size();
  }

  // Returns the offset from the start of the disk to beginning of |pslice| physical slice.
  // Note: pslice is 1-indexed.
  size_t GetSliceStart(size_t pslice) const {
    return 2 * metadata_allocated_size() + (pslice - 1) * slice_size();
  }

  // Returns the maximum number of slices that can be addressed from the maximum possible size of
  // the metatadata.
  size_t GetMaxAllocatableSlices() const {
    // The "-1" here allows for the unused 0 indexed slice.
    // TODO(fxb/59980) the allocation table is 0-indexed (with the 0th entry not used) while the
    // allocation data itself is 1-indexed. This inconsistency should be fixed,
    return (metadata_allocated_size() - kAllocTableOffset) / sizeof(SliceEntry) - 1;
  }

  // Returns the maximum number of slices that the allocated metadata can address for a given
  // |disk_size|.
  size_t GetMaxAddressableSlices(uint64_t disk_size) const {
    size_t slice_count =
        std::min(GetMaxAllocatableSlices(), UsableSlicesCount(disk_size, slice_size()));
    // Because the allocation table is 1-indexed and pslices are 0 indexed on disk, if the number of
    // slices fit perfectly in the metadata, the allocated buffer won't be big enough to address
    // them all. This only happens when the rounded up block value happens to match the disk size.
    // TODO(gevalentino): Fix underlying cause and remove workaround.
    if ((AllocationTable::kOffset + slice_count * sizeof(SliceEntry)) ==
        metadata_allocated_size()) {
      slice_count--;
    }
    return slice_count;
  }

  // Returns the maximum partition size the current metadata can grow to.
  size_t GetMaxPartitionSize() const {
    return GetSliceStart(1) + GetMaxAllocatableSlices() * slice_size();
  }

 private:
  Header header_ = {};
};

// Update's the metadata's hash field to accurately reflect the contents of metadata.
void UpdateHash(void* metadata, size_t metadata_size);

// Validate the FVM header information, and identify which copy of metadata (primary or backup)
// should be used for initial reading, if either.
//
// "out" is an optional output parameter which is equal to a valid copy of either metadata or backup
// on success.
zx_status_t ValidateHeader(const void* metadata, const void* backup, size_t metadata_size,
                           const void** out);

}  // namespace fvm

#endif  // FVM_FVM_H_
