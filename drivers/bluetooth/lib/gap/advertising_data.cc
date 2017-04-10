// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "advertising_data.h"

#include "apps/bluetooth/lib/common/byte_buffer.h"
#include "lib/ftl/logging.h"

namespace bluetooth {
namespace gap {

AdvertisingDataReader::AdvertisingDataReader(const common::ByteBuffer& data)
    : is_valid_(true), ptr_(data.GetData()), remaining_bytes_(data.GetSize()) {
  FTL_DCHECK(ptr_);

  // Do a validity check.
  const uint8_t* ptr = ptr_;
  size_t remaining_bytes = remaining_bytes_;
  while (remaining_bytes) {
    size_t tlv_len = *ptr;

    // A struct can have 0 as its length. In that case its valid to terminate.
    if (!tlv_len) break;

    // The full struct includes the length octet itself.
    size_t struct_size = tlv_len + 1;
    if (struct_size > remaining_bytes) {
      is_valid_ = false;
      break;
    }

    ptr += struct_size;
    remaining_bytes -= struct_size;
  }
}

bool AdvertisingDataReader::GetNextField(DataType* out_type, common::BufferView* out_data) {
  FTL_DCHECK(out_type);
  FTL_DCHECK(out_data);

  if (!HasMoreData()) return false;

  size_t tlv_len = *ptr_;
  size_t cur_struct_size = tlv_len + 1;
  FTL_DCHECK(cur_struct_size <= remaining_bytes_);

  *out_type = static_cast<DataType>(*(ptr_ + 1));
  *out_data = common::BufferView(ptr_ + 2, tlv_len - 1);

  ptr_ += cur_struct_size;
  remaining_bytes_ -= cur_struct_size;

  return true;
}

bool AdvertisingDataReader::HasMoreData() const {
  // If a segment ever begins with 0, then we terminate.
  if (!is_valid_ || !(*ptr_)) return false;
  return remaining_bytes_;
}

}  // namespace gap
}  // namespace bluetooth
