// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include "garnet/lib/overnet/protocol/coding.h"
#include "garnet/lib/overnet/vocabulary/slice.h"
#include "garnet/lib/overnet/vocabulary/status.h"
#include "lib/fidl/cpp/object_coding.h"

namespace overnet {

template <class T>
StatusOr<Slice> Encode(Coding coding, T* object) {
  std::vector<uint8_t> output;
  const char* error_msg;
  if (zx_status_t status = fidl::EncodeObject(object, &output, &error_msg);
      status != ZX_OK) {
    return Status::FromZx(status, error_msg);
  }
  return Encode(coding, Slice::FromContainer(std::move(output)));
}

template <class T>
StatusOr<Slice> Encode(T* object) {
  return Encode(kDefaultCoding, object);
}

template <class T>
StatusOr<T> Decode(Slice update) {
  auto decoded = Decode(std::move(update));
  if (decoded.is_error()) {
    return decoded.AsStatus();
  }
  std::vector<uint8_t> copy(decoded->begin(), decoded->end());
  const char* error_msg;
  T out;
  if (zx_status_t status =
          fidl::DecodeObject(copy.data(), copy.size(), &out, &error_msg);
      status != ZX_OK) {
    return Status::FromZx(status, error_msg);
  }
  return out;
}

}  // namespace overnet
