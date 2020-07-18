// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/zbitl/stdio.h>

#include "src/lib/files/scoped_temp_dir.h"
#include "tests.h"

namespace {

struct FileIo {
  using storage_type = FILE*;

  void Create(std::string_view contents, FILE** zbi) {
    std::string filename;
    ASSERT_TRUE(temp_dir_.NewTempFileWithData(std::string(contents), &filename));
    FILE* f = fopen(filename.c_str(), "r");
    ASSERT_NOT_NULL(f, "cannot open '%s': %s", filename.c_str(), strerror(errno));
    *zbi = f;
  }

  void ReadPayload(FILE* zbi, const zbi_header_t& header, long int payload, std::string* string) {
    string->resize(header.length);
    ASSERT_EQ(0, fseek(zbi, payload, SEEK_SET), "failed to seek to payload: %s", strerror(errno));
    size_t n = fread(string->data(), 1, header.length, zbi);
    ASSERT_EQ(0, ferror(zbi), "failed to read payload: %s", strerror(errno));
    ASSERT_EQ(header.length, n, "did not fully read payload");
  }

  files::ScopedTempDir temp_dir_;
};

// The type of FILE* cannot be default-constructed, so we skip the
// TestDefaultConstructedView() test case.

TEST(ZbitlViewStdioTests, EmptyZbi) { ASSERT_NO_FATAL_FAILURES(TestEmptyZbi<FileIo>()); }

TEST(ZbitlViewStdioTests, SimpleZbi) { ASSERT_NO_FATAL_FAILURES(TestSimpleZbi<FileIo>()); }

TEST(ZbitlViewStdioTests, BadCrcZbi) { ASSERT_NO_FATAL_FAILURES(TestBadCrcZbi<FileIo>()); }

}  // namespace
