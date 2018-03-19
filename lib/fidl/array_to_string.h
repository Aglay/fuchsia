// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_LIB_FIDL_ARRAY_TO_STRING_H_
#define PERIDOT_LIB_FIDL_ARRAY_TO_STRING_H_

#include <string>
#include <vector>

#include "lib/fidl/cpp/bindings/array.h"
#include "lib/fidl/cpp/bindings/string.h"

namespace modular {

inline std::string to_string(const f1dl::VectorPtr<uint8_t>& data) {
  std::string ret;
  ret.reserve(data->size());

  for (uint8_t val : *data) {
    ret += static_cast<char>(val);
  }

  return ret;
}

inline std::string to_hex_string(const f1dl::VectorPtr<uint8_t>& data) {
  constexpr char kHexadecimalCharacters[] = "0123456789abcdef";
  std::string ret;
  ret.reserve(data->size() * 2);
  for (unsigned char i : *data) {
    ret.push_back(kHexadecimalCharacters[i >> 4]);
    ret.push_back(kHexadecimalCharacters[i & 0xf]);
  }
  return ret;
}

inline f1dl::VectorPtr<uint8_t> to_array(const std::string& val) {
  f1dl::VectorPtr<uint8_t> ret;
  for (char c : val) {
    ret.push_back(c);
  }
  return ret;
}

inline f1dl::VectorPtr<f1dl::StringPtr> to_array(const std::vector<std::string>& val) {
  f1dl::VectorPtr<f1dl::StringPtr> ret;
  ret.resize(0);  // mark as not null
  for (const std::string& s : val) {
    ret.push_back(s);
  }
  return ret;
}

}  // namespace modular

#endif
