// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/ledger/tests/integration/test_utils.h"

#include <zx/time.h>

#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "lib/fidl/cpp/bindings/binding.h"
#include "lib/fsl/tasks/message_loop.h"
#include "lib/fsl/vmo/strings.h"
#include "lib/fxl/files/scoped_temp_dir.h"
#include "lib/fxl/macros.h"
#include "lib/fxl/time/time_delta.h"
#include "lib/ledger/fidl/ledger.fidl.h"
#include "peridot/bin/ledger/app/ledger_repository_factory_impl.h"
#include "peridot/bin/ledger/fidl/internal.fidl.h"
#include "peridot/lib/convert/convert.h"

namespace test {
namespace integration {

f1dl::VectorPtr<uint8_t> RandomArray(size_t size,
                                 const std::vector<uint8_t>& prefix) {
  EXPECT_TRUE(size >= prefix.size());
  f1dl::VectorPtr<uint8_t> array = f1dl::VectorPtr<uint8_t>::New(size);
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

f1dl::VectorPtr<uint8_t> RandomArray(int size) {
  return RandomArray(size, std::vector<uint8_t>());
}

f1dl::VectorPtr<uint8_t> PageGetId(ledger::PagePtr* page) {
  f1dl::VectorPtr<uint8_t> page_id;
  (*page)->GetId(
      [&page_id](f1dl::VectorPtr<uint8_t> id) { page_id = std::move(id); });
  EXPECT_TRUE(page->WaitForResponseUntil(zx::deadline_after(zx::sec(1))));
  return page_id;
}

ledger::PageSnapshotPtr PageGetSnapshot(ledger::PagePtr* page,
                                        f1dl::VectorPtr<uint8_t> prefix) {
  ledger::PageSnapshotPtr snapshot;
  (*page)->GetSnapshot(
      snapshot.NewRequest(), std::move(prefix), nullptr,
      [](ledger::Status status) { EXPECT_EQ(ledger::Status::OK, status); });
  EXPECT_TRUE(page->WaitForResponseUntil(zx::deadline_after(zx::sec(1))));
  return snapshot;
}

f1dl::VectorPtr<f1dl::VectorPtr<uint8_t>> SnapshotGetKeys(
    ledger::PageSnapshotPtr* snapshot,
    f1dl::VectorPtr<uint8_t> start) {
  return SnapshotGetKeys(snapshot, std::move(start), nullptr);
}

f1dl::VectorPtr<f1dl::VectorPtr<uint8_t>> SnapshotGetKeys(
    ledger::PageSnapshotPtr* snapshot,
    f1dl::VectorPtr<uint8_t> start,
    int* num_queries) {
  f1dl::VectorPtr<f1dl::VectorPtr<uint8_t>> result;
  f1dl::VectorPtr<uint8_t> token = nullptr;
  f1dl::VectorPtr<uint8_t> next_token = nullptr;
  if (num_queries) {
    *num_queries = 0;
  }
  do {
    (*snapshot)->GetKeys(
        start.Clone(), std::move(token),
        [&result, &next_token, &num_queries](
            ledger::Status status, f1dl::VectorPtr<f1dl::VectorPtr<uint8_t>> keys,
            f1dl::VectorPtr<uint8_t> new_next_token) {
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
    EXPECT_TRUE(snapshot->WaitForResponseUntil(zx::deadline_after(zx::sec(1))));
    token = std::move(next_token);
    next_token = nullptr;  // Suppress misc-use-after-move.
  } while (token);
  return result;
}

f1dl::VectorPtr<ledger::EntryPtr> SnapshotGetEntries(
    ledger::PageSnapshotPtr* snapshot,
    f1dl::VectorPtr<uint8_t> start) {
  return SnapshotGetEntries(snapshot, std::move(start), nullptr);
}

f1dl::VectorPtr<ledger::EntryPtr> SnapshotGetEntries(
    ledger::PageSnapshotPtr* snapshot,
    f1dl::VectorPtr<uint8_t> start,
    int* num_queries) {
  f1dl::VectorPtr<ledger::EntryPtr> result;
  f1dl::VectorPtr<uint8_t> token = nullptr;
  f1dl::VectorPtr<uint8_t> next_token = nullptr;
  if (num_queries) {
    *num_queries = 0;
  }
  do {
    (*snapshot)->GetEntries(
        start.Clone(), std::move(token),
        [&result, &next_token, &num_queries](
            ledger::Status status, f1dl::VectorPtr<ledger::EntryPtr> entries,
            f1dl::VectorPtr<uint8_t> new_next_token) {
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
    EXPECT_TRUE(snapshot->WaitForResponseUntil(zx::deadline_after(zx::sec(1))));
    token = std::move(next_token);
    next_token = nullptr;  // Suppress misc-use-after-move.
  } while (token);
  return result;
}

std::string ToString(const fsl::SizedVmoTransportPtr& vmo) {
  std::string value;
  bool status = fsl::StringFromVmo(vmo, &value);
  FXL_DCHECK(status);
  return value;
}

f1dl::VectorPtr<uint8_t> ToArray(const fsl::SizedVmoTransportPtr& vmo) {
  return convert::ToArray(ToString(vmo));
}

std::string SnapshotFetchPartial(ledger::PageSnapshotPtr* snapshot,
                                 f1dl::VectorPtr<uint8_t> key,
                                 int64_t offset,
                                 int64_t max_size) {
  std::string result;
  (*snapshot)->FetchPartial(
      std::move(key), offset, max_size,
      [&result](ledger::Status status, fsl::SizedVmoTransportPtr buffer) {
        EXPECT_EQ(ledger::Status::OK, status);
        EXPECT_TRUE(fsl::StringFromVmo(buffer, &result));
      });
  EXPECT_TRUE(snapshot->WaitForResponseUntil(zx::deadline_after(zx::sec(1))));
  return result;
}

}  // namespace integration
}  // namespace test
