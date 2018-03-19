// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_CLOUD_PROVIDER_VALIDATION_CONVERT_H_
#define LIB_CLOUD_PROVIDER_VALIDATION_CONVERT_H_

#include <string>

#include "lib/fidl/cpp/bindings/array.h"

namespace cloud_provider {

f1dl::VectorPtr<uint8_t> ToArray(const std::string& val);

std::string ToString(const f1dl::VectorPtr<uint8_t>& bytes);

std::string ToHex(const f1dl::VectorPtr<uint8_t>& bytes);

}  // namespace cloud_provider

#endif  // LIB_CLOUD_PROVIDER_VALIDATION_CONVERT_H_
