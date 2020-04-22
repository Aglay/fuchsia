// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_TESTS_BENCHMARKS_FIDL_LLCPP_DECODE_BENCHMARK_UTIL_H_
#define SRC_TESTS_BENCHMARKS_FIDL_LLCPP_DECODE_BENCHMARK_UTIL_H_

#include "encode_benchmark_util.h"

namespace llcpp_benchmarks {

template <typename FidlType>
bool DecodeBenchmark(perftest::RepeatState* state, fidl::aligned<FidlType>* aligned_value) {
  static_assert(fidl::IsFidlType<FidlType>::value, "FIDL type required");

  // Encode the value.
  uint8_t linearize_buffer[BufferSize<FidlType>];
  auto benchmark_linearize_result = Linearize(nullptr, &aligned_value->value, linearize_buffer);
  auto& linearize_result = benchmark_linearize_result.result;
  ZX_ASSERT(linearize_result.status == ZX_OK && linearize_result.error == nullptr);
  auto encode_result = fidl::Encode(std::move(linearize_result.message));
  ZX_ASSERT(encode_result.status == ZX_OK && encode_result.error == nullptr);
  const fidl::BytePart& bytes = encode_result.message.bytes();

  state->DeclareStep("Setup/WallTime");
  state->DeclareStep("Decode/WallTime");
  state->DeclareStep("Destructors/WallTime");

  std::vector<uint8_t> test_data(bytes.size());
  while (state->KeepRunning()) {
    // TODO(fxb/49815) Move the memcpy out of the main loop.
    memcpy(test_data.data(), bytes.data(), bytes.size());
    fidl::EncodedMessage<FidlType> message(
        fidl::BytePart(&test_data[0], test_data.size(), test_data.size()));

    state->NextStep();  // Done with setup. Next: Decode.

    auto decode_result = fidl::Decode(std::move(message));
    ZX_ASSERT(decode_result.status == ZX_OK);

    state->NextStep();  // Next: Destructors called.
  }
  return true;
}

}  // namespace llcpp_benchmarks

#endif  // SRC_TESTS_BENCHMARKS_FIDL_LLCPP_DECODE_BENCHMARK_UTIL_H_
