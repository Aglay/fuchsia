// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <lib/zbitl/fd.h>

#include "src/lib/files/scoped_temp_dir.h"
#include "tests.h"

namespace {

struct FdIo {
  using storage_type = fbl::unique_fd;

  void Create(std::string_view contents, fbl::unique_fd* zbi) {
    std::string filename;
    ASSERT_TRUE(temp_dir_.NewTempFileWithData(std::string(contents), &filename));
    fbl::unique_fd fd{open(filename.c_str(), O_RDWR)};
    ASSERT_TRUE(fd, "cannot open '%s': %s", filename.c_str(), strerror(errno));
    *zbi = std::move(fd);
  }

  void ReadPayload(const fbl::unique_fd& zbi, const zbi_header_t& header, off_t payload,
                   std::string* string) {
    string->resize(header.length);
    ssize_t n = pread(zbi.get(), string->data(), header.length, payload);
    ASSERT_GT(n, 0, "error encountered: %s", strerror(errno));
    ASSERT_EQ(header.length, static_cast<uint32_t>(n), "did not fully read payload");
  }

  files::ScopedTempDir temp_dir_;
};

TEST(ZbitlViewFdTests, DefaultConstructed) {
  ASSERT_NO_FATAL_FAILURES(TestDefaultConstructedView<FdIo>(true));
}

TEST(ZbitlViewFdTests, EmptyZbi) { ASSERT_NO_FATAL_FAILURES(TestEmptyZbi<FdIo>()); }

TEST(ZbitlViewFdTests, SimpleZbi) { ASSERT_NO_FATAL_FAILURES(TestSimpleZbi<FdIo>()); }

TEST(ZbitlViewFdTests, BadCrcZbi) { ASSERT_NO_FATAL_FAILURES(TestBadCrcZbi<FdIo>()); }

TEST(ZbitlViewFdTests, Mutation) { ASSERT_NO_FATAL_FAILURES(TestMutation<FdIo>()); }

}  // namespace
