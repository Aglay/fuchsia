// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lib/fidl/cpp/string.h"

#include <string.h>

#include "lib/fidl/cpp/encoder.h"

namespace fidl {

StringPtr::StringPtr() : is_null_if_empty_(true) {}

StringPtr::StringPtr(const StringPtr& other) = default;

StringPtr::StringPtr(std::string str)
    : str_(std::move(str)), is_null_if_empty_(false) {}

StringPtr::StringPtr(const char* str)
    : str_(str ? std::string(str) : std::string()), is_null_if_empty_(!str) {}

StringPtr::StringPtr(const char* str, size_t length)
    : str_(str ? std::string(str, length) : std::string()),
      is_null_if_empty_(!str) {}

StringPtr::~StringPtr() = default;

StringPtr::StringPtr(StringPtr&& other)
    : str_(std::move(other.str_)), is_null_if_empty_(other.is_null_if_empty_) {}

StringPtr& StringPtr::operator=(const StringPtr& other) = default;

StringPtr& StringPtr::operator=(StringPtr&& other) {
  str_ = std::move(other.str_);
  is_null_if_empty_ = other.is_null_if_empty_;
  return *this;
}

void StringPtr::Encode(Encoder* encoder, size_t offset) {
  if (is_null()) {
    fidl_string_t* string = encoder->GetPtr<fidl_string_t>(offset);
    string->size = 0u;
    string->data = reinterpret_cast<char*>(FIDL_ALLOC_ABSENT);
  } else {
    EncodeString(encoder, str_, offset);
  }
}

void StringPtr::Decode(Decoder* decoder, StringPtr* value, size_t offset) {
  fidl_string_t* string = decoder->GetPtr<fidl_string_t>(offset);
  if (string->data) {
    value->reset(std::string(string->data, string->size));
  } else {
    *value = StringPtr();
  }
}

}  // namespace fidl
