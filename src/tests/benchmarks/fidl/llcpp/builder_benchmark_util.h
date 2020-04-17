// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_TESTS_BENCHMARKS_FIDL_LLCPP_BUILDER_BENCHMARK_UTIL_H_
#define SRC_TESTS_BENCHMARKS_FIDL_LLCPP_BUILDER_BENCHMARK_UTIL_H_

namespace llcpp_benchmarks {

template <typename BuilderFunc>
bool BuilderBenchmark(perftest::RepeatState* state, BuilderFunc builder) {
  state->DeclareStep("Build/WallTime");
  state->DeclareStep("Destructors/WallTime");

  while (state->KeepRunning()) {
    builder(state);
  }

  return true;
}

template <typename Allocator, typename BuilderFunc>
bool BuilderBenchmark(perftest::RepeatState* state, BuilderFunc builder) {
  state->DeclareStep("CreateAllocator/WallTime");
  state->DeclareStep("Build/WallTime");
  state->DeclareStep("Destructors/WallTime");

  while (state->KeepRunning()) {
    Allocator allocator;
    state->NextStep();
    builder(state, &allocator);
  }

  return true;
}

}  // namespace llcpp_benchmarks

#endif  // SRC_TESTS_BENCHMARKS_FIDL_LLCPP_BUILDER_BENCHMARK_UTIL_H_
