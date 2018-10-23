// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/guest/vmm/device/block_dispatcher.h"

#include <gtest/gtest.h>

#include "garnet/lib/machina/device/block.h"

namespace {

static constexpr size_t kDispatcherSize = 8 * 1024 * 1024;

using BufVector = std::vector<uint8_t>;

// Read only dispatcher that returns blocks containing a single byte.
class StaticDispatcher : public BlockDispatcher {
 public:
  void Sync(Callback callback) override { callback(ZX_OK); }

  void ReadAt(void* data, uint64_t size, uint64_t off,
              Callback callback) override {
    memset(data, value_, size);
    callback(ZX_OK);
  }

  void WriteAt(const void* data, uint64_t size, uint64_t off,
               Callback callback) override {
    callback(ZX_ERR_NOT_SUPPORTED);
  }

 private:
  uint8_t value_ = 0xab;
};

#define ASSERT_BLOCK_VALUE(ptr, size, val) \
  do {                                     \
    for (size_t i = 0; i < (size); ++i) {  \
      ASSERT_EQ((val), (ptr)[i]);          \
    }                                      \
  } while (false)

std::unique_ptr<BlockDispatcher> CreateDispatcher() {
  std::unique_ptr<BlockDispatcher> disp;
  CreateVolatileWriteBlockDispatcher(
      kDispatcherSize, std::make_unique<StaticDispatcher>(),
      [&disp](size_t size, std::unique_ptr<BlockDispatcher> in) {
        disp = std::move(in);
      });
  return disp;
}

TEST(VolatileWriteBlockDispatcherTest, WriteBlock) {
  auto disp = CreateDispatcher();

  zx_status_t status;
  fidl::VectorPtr<uint8_t> buf(machina::kBlockSectorSize);
  disp->ReadAt(buf->data(), buf->size(), 0,
               [&status](zx_status_t s) { s = status; });
  ASSERT_EQ(ZX_OK, status);
  ASSERT_BLOCK_VALUE(buf->data(), buf->size(), 0xab);

  fidl::VectorPtr<uint8_t> write_buf(
      BufVector(machina::kBlockSectorSize, 0xbe));
  disp->WriteAt(write_buf->data(), write_buf->size(), 0,
                [&status](zx_status_t s) { status = s; });
  ASSERT_EQ(ZX_OK, status);

  disp->ReadAt(buf->data(), buf->size(), 0,
               [&status](zx_status_t s) { s = status; });
  ASSERT_EQ(ZX_OK, status);
  ASSERT_BLOCK_VALUE(buf->data(), buf->size(), 0xbe);
}

TEST(VolatileWriteBlockDispatcherTest, WriteBlockComplex) {
  auto disp = CreateDispatcher();

  // Write blocks 0 & 2, blocks 1 & 3 will hit the static dispatcher.
  fidl::VectorPtr<uint8_t> write_buf(
      BufVector(machina::kBlockSectorSize, 0xbe));
  zx_status_t status;
  disp->WriteAt(write_buf->data(), write_buf->size(), 0,
                [&status](zx_status_t s) { status = s; });
  ASSERT_EQ(ZX_OK, status);
  disp->WriteAt(write_buf->data(), write_buf->size(),
                machina::kBlockSectorSize * 2,
                [&status](zx_status_t s) { status = s; });
  ASSERT_EQ(ZX_OK, status);

  fidl::VectorPtr<uint8_t> buf(machina::kBlockSectorSize * 4);
  disp->ReadAt(buf->data(), buf->size(), 0,
               [&status](zx_status_t s) { s = status; });
  ASSERT_EQ(ZX_OK, status);
  ASSERT_BLOCK_VALUE(buf->data(), machina::kBlockSectorSize, 0xbe);
  ASSERT_BLOCK_VALUE(buf->data() + machina::kBlockSectorSize,
                     machina::kBlockSectorSize, 0xab);
  ASSERT_BLOCK_VALUE(buf->data() + machina::kBlockSectorSize * 2,
                     machina::kBlockSectorSize, 0xbe);
  ASSERT_BLOCK_VALUE(buf->data() + machina::kBlockSectorSize * 3,
                     machina::kBlockSectorSize, 0xab);
}

TEST(VolatileWriteBlockDispatcherTest, BadRequest) {
  auto disp = CreateDispatcher();

  zx_status_t status;
  disp->ReadAt(nullptr, machina::kBlockSectorSize, 1,
               [&status](zx_status_t s) { status = s; });
  EXPECT_EQ(ZX_ERR_INVALID_ARGS, status);

  disp->ReadAt(nullptr, machina::kBlockSectorSize - 1, 0,
               [&status](zx_status_t s) { status = s; });
  EXPECT_EQ(ZX_ERR_INVALID_ARGS, status);

  disp->WriteAt(nullptr, machina::kBlockSectorSize, 1,
                [&status](zx_status_t s) { status = s; });
  EXPECT_EQ(ZX_ERR_INVALID_ARGS, status);

  disp->WriteAt(nullptr, machina::kBlockSectorSize - 1, 0,
                [&status](zx_status_t s) { status = s; });
  EXPECT_EQ(ZX_ERR_INVALID_ARGS, status);
}

}  // namespace