// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_STORAGE_VOLUME_IMAGE_FVM_PARTITION_H_
#define SRC_STORAGE_VOLUME_IMAGE_FVM_PARTITION_H_

#include <memory>

#include "src/storage/volume_image/block_io.h"
#include "src/storage/volume_image/fvm/address_descriptor.h"
#include "src/storage/volume_image/volume_descriptor.h"

namespace storage::volume_image {

// A Partition consists of the volume descriptor, allowing the fvm to know how the partition should
// look, an address descriptor allowing the fvm to know how the volume data should be moved in the
// fvm address space and last a reader, which provides access to the volume data in the volume
// address space.
//
// This class is move constructible and move assignable only.
// This class is thread-compatible.
class Partition {
 public:
  Partition() = default;
  Partition(VolumeDescriptor volume_descriptor, AddressDescriptor address_descriptor,
            std::unique_ptr<BlockReader> reader)
      : volume_(std::move(volume_descriptor)),
        address_(std::move(address_descriptor)),
        reader_(std::move(reader)) {}
  Partition(const Partition&) = delete;
  Partition(Partition&&) = default;
  Partition& operator=(const Partition&) = delete;
  Partition& operator=(Partition&&) = default;
  ~Partition() = default;

  // Returns the volume descriptor for this partition.
  const VolumeDescriptor& volume() const { return volume_; }

  // Returns the address descriptor for this partition.
  const AddressDescriptor& address() const { return address_; }

  // Returns the reader for this partition, which allows reading the volume data from the source
  // address space.
  const BlockReader* reader() const { return reader_.get(); }

 private:
  // Information about the volume in this partition.
  VolumeDescriptor volume_ = {};

  // Information about the address or extents in this partitions and how to map them to target
  // space.
  AddressDescriptor address_ = {};

  // Mechanism for reading volume data.
  std::unique_ptr<BlockReader> reader_ = nullptr;
};

}  // namespace storage::volume_image

#endif  // SRC_STORAGE_VOLUME_IMAGE_FVM_PARTITION_H_
