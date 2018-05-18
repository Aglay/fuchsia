// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/ledger/tests/integration/test_utils.h"

#include <string>
#include <utility>
#include <vector>

#include <ledger/cpp/fidl.h>
#include <ledger_internal/cpp/fidl.h>
#include <lib/zx/time.h>

#include "gtest/gtest.h"
#include "lib/fidl/cpp/binding.h"
#include "lib/fsl/vmo/strings.h"
#include "lib/fxl/files/scoped_temp_dir.h"
#include "lib/fxl/macros.h"
#include "lib/fxl/time/time_delta.h"
#include "peridot/lib/convert/convert.h"

namespace test {
namespace integration {

fidl::VectorPtr<uint8_t> RandomArray(size_t size,
                                     const std::vector<uint8_t>& prefix) {
  EXPECT_TRUE(size >= prefix.size());
  fidl::VectorPtr<uint8_t> array = fidl::VectorPtr<uint8_t>::New(size);
  for (size_t i = 0; i < prefix.size(); ++i) {
    array->at(i) = prefix[i];
  }
  for (size_t i = prefix.size(); i < size / 4; ++i) {
    int random = std::rand();
    for (size_t j = 0; j < 4 && 4 * i + j < size; ++j) {
      array->at(4 * i + j) = random & 0xFF;
      random = random >> 8;
    }
  }
  return array;
}

fidl::VectorPtr<uint8_t> RandomArray(int size) {
  return RandomArray(size, std::vector<uint8_t>());
}

ledger::PageId PageGetId(ledger::PagePtr* page) {
  ledger::PageId page_id;
  (*page)->GetId([&page_id](ledger::PageId id) { page_id = std::move(id); });
  EXPECT_EQ(ZX_OK, page->WaitForResponseUntil(zx::deadline_after(zx::sec(1))));
  return page_id;
}

ledger::PageSnapshotPtr PageGetSnapshot(ledger::PagePtr* page,
                                        fidl::VectorPtr<uint8_t> prefix) {
  ledger::PageSnapshotPtr snapshot;
  (*page)->GetSnapshot(
      snapshot.NewRequest(), std::move(prefix), nullptr,
      [](ledger::Status status) { EXPECT_EQ(ledger::Status::OK, status); });
  EXPECT_EQ(ZX_OK, page->WaitForResponseUntil(zx::deadline_after(zx::sec(1))));
  return snapshot;
}

fidl::VectorPtr<fidl::VectorPtr<uint8_t>> SnapshotGetKeys(
    ledger::PageSnapshotPtr* snapshot,
    fidl::VectorPtr<uint8_t> start) {
  return SnapshotGetKeys(snapshot, std::move(start), nullptr);
}

fidl::VectorPtr<fidl::VectorPtr<uint8_t>> SnapshotGetKeys(
    ledger::PageSnapshotPtr* snapshot,
    fidl::VectorPtr<uint8_t> start,
    int* num_queries) {
  fidl::VectorPtr<fidl::VectorPtr<uint8_t>> result;
  fidl::VectorPtr<uint8_t> token = nullptr;
  fidl::VectorPtr<uint8_t> next_token = nullptr;
  if (num_queries) {
    *num_queries = 0;
  }
  do {
    (*snapshot)->GetKeys(
        start.Clone(), std::move(token),
        [&result, &next_token, &num_queries](
            ledger::Status status,
            fidl::VectorPtr<fidl::VectorPtr<uint8_t>> keys,
            fidl::VectorPtr<uint8_t> new_next_token) {
          EXPECT_TRUE(status == ledger::Status::OK ||
                      status == ledger::Status::PARTIAL_RESULT);
          if (num_queries) {
            (*num_queries)++;
          }
          for (auto& key : keys.take()) {
            result.push_back(std::move(key));
          }
          next_token = std::move(new_next_token);
        });
    EXPECT_EQ(ZX_OK,
              snapshot->WaitForResponseUntil(zx::deadline_after(zx::sec(1))));
    token = std::move(next_token);
    next_token = nullptr;  // Suppress misc-use-after-move.
  } while (token);
  return result;
}

fidl::VectorPtr<ledger::Entry> SnapshotGetEntries(
    ledger::PageSnapshotPtr* snapshot,
    fidl::VectorPtr<uint8_t> start) {
  return SnapshotGetEntries(snapshot, std::move(start), nullptr);
}

fidl::VectorPtr<ledger::Entry> SnapshotGetEntries(
    ledger::PageSnapshotPtr* snapshot,
    fidl::VectorPtr<uint8_t> start,
    int* num_queries) {
  fidl::VectorPtr<ledger::Entry> result;
  fidl::VectorPtr<uint8_t> token = nullptr;
  fidl::VectorPtr<uint8_t> next_token = nullptr;
  if (num_queries) {
    *num_queries = 0;
  }
  do {
    (*snapshot)->GetEntries(
        start.Clone(), std::move(token),
        [&result, &next_token, &num_queries](
            ledger::Status status, fidl::VectorPtr<ledger::Entry> entries,
            fidl::VectorPtr<uint8_t> new_next_token) {
          EXPECT_TRUE(status == ledger::Status::OK ||
                      status == ledger::Status::PARTIAL_RESULT)
              << "Actual status: " << status;
          if (num_queries) {
            (*num_queries)++;
          }
          for (auto& entry : entries.take()) {
            result.push_back(std::move(entry));
          }
          next_token = std::move(new_next_token);
        });
    EXPECT_EQ(ZX_OK,
              snapshot->WaitForResponseUntil(zx::deadline_after(zx::sec(1))));
    token = std::move(next_token);
    next_token = nullptr;  // Suppress misc-use-after-move.
  } while (token);
  return result;
}

std::string ToString(const fuchsia::mem::BufferPtr& vmo) {
  std::string value;
  bool status = fsl::StringFromVmo(*vmo, &value);
  FXL_DCHECK(status);
  return value;
}

fidl::VectorPtr<uint8_t> ToArray(const fuchsia::mem::BufferPtr& vmo) {
  return convert::ToArray(ToString(vmo));
}

std::string SnapshotFetchPartial(ledger::PageSnapshotPtr* snapshot,
                                 fidl::VectorPtr<uint8_t> key,
                                 int64_t offset,
                                 int64_t max_size) {
  std::string result;
  (*snapshot)->FetchPartial(
      std::move(key), offset, max_size,
      [&result](ledger::Status status, fuchsia::mem::BufferPtr buffer) {
        EXPECT_EQ(ledger::Status::OK, status);
        EXPECT_TRUE(fsl::StringFromVmo(*buffer, &result));
      });
  EXPECT_EQ(ZX_OK,
            snapshot->WaitForResponseUntil(zx::deadline_after(zx::sec(1))));
  return result;
}

}  // namespace integration
}  // namespace test
