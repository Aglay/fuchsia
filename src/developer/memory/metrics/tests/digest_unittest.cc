// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/memory/metrics/digest.h"

#include <gtest/gtest.h>

#include "src/developer/memory/metrics/tests/test_utils.h"

namespace memory {
namespace test {

using DigestUnitTest = testing::Test;

struct ExpectedBucket {
  std::string name;
  uint64_t size;
};

void ConfirmBuckets(const Digest& digest, const std::vector<ExpectedBucket>& expected_buckets) {
  auto const& buckets = digest.buckets();
  ASSERT_EQ(expected_buckets.size(), buckets.size());
  for (size_t i = 0; i < expected_buckets.size(); i++) {
    const auto& expected_bucket = expected_buckets.at(i);
    const auto& bucket = buckets.at(i);

    EXPECT_STREQ(expected_bucket.name.c_str(), bucket.name().c_str());
    EXPECT_EQ(expected_bucket.size, bucket.size());
  }
}

TEST_F(DigestUnitTest, VMONames) {
  Capture c;
  TestUtils::CreateCapture(&c, {
                                   .vmos =
                                       {
                                           {.koid = 1, .name = "a1", .committed_bytes = 100},
                                           {.koid = 2, .name = "b1", .committed_bytes = 200},
                                       },
                                   .processes =
                                       {
                                           {.koid = 1, .name = "p1", .vmos = {1}},
                                           {.koid = 2, .name = "q1", .vmos = {2}},
                                       },
                               });

  Digest d(c, {{"A", ".*", "a.*"}, {"B", ".*", "b.*"}});
  ConfirmBuckets(d, {{"B", 200U}, {"A", 100U}});
  EXPECT_EQ(0U, d.undigested_vmos().size());
}  // namespace test

TEST_F(DigestUnitTest, ProcessNames) {
  Capture c;
  TestUtils::CreateCapture(&c, {
                                   .vmos =
                                       {
                                           {.koid = 1, .name = "a1", .committed_bytes = 100},
                                           {.koid = 2, .name = "b1", .committed_bytes = 200},
                                       },
                                   .processes =
                                       {
                                           {.koid = 1, .name = "p1", .vmos = {1}},
                                           {.koid = 2, .name = "q1", .vmos = {2}},
                                       },
                               });

  Digest d(c, {{"P", "p.*", ".*"}, {"Q", "q.*", ".*"}});
  ConfirmBuckets(d, {{"Q", 200U}, {"P", 100U}});
  EXPECT_EQ(0U, d.undigested_vmos().size());
}

TEST_F(DigestUnitTest, Undigested) {
  Capture c;
  TestUtils::CreateCapture(&c, {
                                   .vmos =
                                       {
                                           {.koid = 1, .name = "a1", .committed_bytes = 100},
                                           {.koid = 2, .name = "b1", .committed_bytes = 200},
                                       },
                                   .processes =
                                       {
                                           {.koid = 1, .name = "p1", .vmos = {1}},
                                           {.koid = 2, .name = "q1", .vmos = {2}},
                                       },
                               });

  Digest d(c, {{"A", ".*", "a.*"}});
  ASSERT_EQ(1U, d.undigested_vmos().size());
  ASSERT_NE(d.undigested_vmos().end(), d.undigested_vmos().find(2U));
  ConfirmBuckets(d, {{"A", 100U}, {"Undigested", 200U}});
}  // namespace test

TEST_F(DigestUnitTest, Kernel) {
  // Test kernel stats.
  Capture c;
  TestUtils::CreateCapture(&c, {
                                   .kmem =
                                       {
                                           .total_bytes = 1000,
                                           .wired_bytes = 10,
                                           .total_heap_bytes = 20,
                                           .mmu_overhead_bytes = 30,
                                           .ipc_bytes = 40,
                                           .other_bytes = 50,
                                           .free_bytes = 100,
                                       },
                               });
  Digest d(c, {});
  EXPECT_EQ(0U, d.undigested_vmos().size());
  ConfirmBuckets(d, {{"Kernel", 150U}, {"Free", 100U}});
}

TEST_F(DigestUnitTest, Orphaned) {
  // Test kernel stats.
  Capture c;
  TestUtils::CreateCapture(&c, {
                                   .kmem =
                                       {
                                           .total_bytes = 1000,
                                           .vmo_bytes = 300,
                                       },
                                   .vmos =
                                       {
                                           {.koid = 1, .name = "a1", .committed_bytes = 100},
                                       },
                                   .processes =
                                       {
                                           {.koid = 1, .name = "p1", .vmos = {1}},
                                       },
                               });
  Digest d(c, {{"A", ".*", "a.*"}});
  EXPECT_EQ(0U, d.undigested_vmos().size());
  ConfirmBuckets(d, {{"A", 100U}, {"Orphaned", 200U}, {"Kernel", 0U}, {"Free", 0U}});
}

TEST_F(DigestUnitTest, DefaultBuckets) {
  // Test kernel stats.
  Capture c;
  TestUtils::CreateCapture(
      &c, {
              .vmos =
                  {
                      {.koid = 1, .name = "", .committed_bytes = 1},
                      {.koid = 2, .name = "magma_create_buffer", .committed_bytes = 2},
                      {.koid = 3, .name = "Sysmem:buf", .committed_bytes = 3},
                      {.koid = 4, .name = "test", .committed_bytes = 4},
                      {.koid = 5, .name = "test", .committed_bytes = 5},
                      {.koid = 6, .name = "test", .committed_bytes = 6},
                      {.koid = 7, .name = "test", .committed_bytes = 7},
                      {.koid = 8, .name = "test", .committed_bytes = 8},
                      {.koid = 9, .name = "test", .committed_bytes = 9},
                      {.koid = 10, .name = "test", .committed_bytes = 10},
                      {.koid = 11, .name = "test", .committed_bytes = 11},
                      {.koid = 12, .name = "test", .committed_bytes = 12},
                      {.koid = 13, .name = "test", .committed_bytes = 13},
                      {.koid = 14, .name = "test", .committed_bytes = 14},
                      {.koid = 15, .name = "test", .committed_bytes = 15},
                      {.koid = 16, .name = "test", .committed_bytes = 16},
                      {.koid = 17, .name = "test", .committed_bytes = 17},
                  },
              .processes =
                  {
                      {.koid = 1, .name = "bin/bootsvc", .vmos = {1}},
                      {.koid = 2, .name = "test", .vmos = {2}},
                      {.koid = 3, .name = "devhost:sys", .vmos = {3}},
                      {.koid = 4, .name = "minfs:/data", .vmos = {4}},
                      {.koid = 5, .name = "blobfs:/blob", .vmos = {5}},
                      {.koid = 6, .name = "io.flutter.product_runner.jit", .vmos = {6}},
                      {.koid = 7, .name = "kronk.cmx", .vmos = {7}},
                      {.koid = 8, .name = "scenic.cmx", .vmos = {8}},
                      {.koid = 9, .name = "devhost:pdev:05:00:f", .vmos = {9}},
                      {.koid = 10, .name = "netstack.cmx", .vmos = {10}},
                      {.koid = 11, .name = "amber.cmx", .vmos = {11}},
                      {.koid = 12, .name = "pkgfs", .vmos = {12}},
                      {.koid = 13, .name = "cast_agent.cmx", .vmos = {13}},
                      {.koid = 14, .name = "web_engine_exe:renderer", .vmos = {14}},
                      {.koid = 15, .name = "web_engine_exe:gpu", .vmos = {15}},
                      {.koid = 16, .name = "chromium.cmx", .vmos = {16}},
                      {.koid = 17, .name = "new", .vmos = {17}},
                  },
          });
  Digest d(c);
  EXPECT_EQ(1U, d.undigested_vmos().size());

  ConfirmBuckets(d, {
                        {"Web", 45U},
                        {"Cast", 13},
                        {"Pkgfs", 12},
                        {"Amber", 11U},
                        {"Netstack", 10U},
                        {"Amlogic", 9U},
                        {"Scenic", 8U},
                        {"Kronk", 7U},
                        {"Opal", 6U},
                        {"Blobfs", 5U},
                        {"Minfs", 4U},
                        {"Video Buffer", 3U},
                        {"Graphics", 2U},
                        {"ZBI Buffer", 1U},
                        {"Undigested", 17U},
                    });
}

}  // namespace test
}  // namespace memory
