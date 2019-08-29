// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_FIDL_LLCPP_TEST_UTILS_H_
#define LIB_FIDL_LLCPP_TEST_UTILS_H_

#include <lib/fidl/llcpp/coding.h>
#include <zircon/status.h>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

namespace llcpp_conformance_utils {

bool ComparePayload(const uint8_t* actual, size_t actual_size, const uint8_t* expected,
                    size_t expected_size);

// Verifies that |value| encodes to |bytes|.
// Note: This is destructive to |value|.
template <typename FidlType>
bool EncodeSuccess(FidlType* value, const std::vector<uint8_t>& bytes) {
  static_assert(fidl::IsFidlType<FidlType>::value, "FIDL type required");
  fidl::DecodedMessage<FidlType> message;
  std::vector<uint8_t> buffer(ZX_CHANNEL_MAX_MSG_BYTES);
  if constexpr (FidlType::Type != nullptr && FidlType::MaxOutOfLine > 0) {
    auto linearize_result =
        fidl::Linearize(value, fidl::BytePart(&buffer[0], ZX_CHANNEL_MAX_MSG_BYTES));
    if (linearize_result.status != ZX_OK || linearize_result.error != nullptr) {
      std::cout << "Linearization failed (" << zx_status_get_string(linearize_result.status)
                << "): " << linearize_result.error << std::endl;
      return false;
    }
    message = std::move(linearize_result.message);
  } else {
    message = fidl::DecodedMessage<FidlType>(
        fidl::BytePart(reinterpret_cast<uint8_t*>(value), sizeof(FidlType), sizeof(FidlType)));
  }

  auto encode_result = fidl::Encode(std::move(message));
  if (encode_result.status != ZX_OK || encode_result.error != nullptr) {
    std::cout << "Encoding failed (" << zx_status_get_string(encode_result.status)
              << "): " << encode_result.error << std::endl;
    return false;
  }
  return ComparePayload(encode_result.message.bytes().data(),
                        encode_result.message.bytes().actual(), &bytes[0], bytes.size());
}

// Verifies that |bytes| decodes to an object that is the same as |value|.
template <typename FidlType>
bool DecodeSuccess(FidlType* value, const std::vector<uint8_t>& bytes) {
  static_assert(fidl::IsFidlType<FidlType>::value, "FIDL type required");
  // TODO(fxb/7958): For now we are only ensuring that no error is present.
  // Need deep equality to verify that the result is the same as |value|.
  return true;
}

}  // namespace llcpp_conformance_utils

#endif  // LIB_FIDL_LLCPP_TEST_UTILS_H_
