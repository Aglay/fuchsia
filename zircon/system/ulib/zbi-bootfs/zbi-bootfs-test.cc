// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <getopt.h>
#include <lib/fzl/fdio.h>
#include <lib/zx/vmo.h>
#include <stdlib.h>
#include <zircon/boot/image.h>
#include <zircon/process.h>
#include <zircon/status.h>
#include <zircon/syscalls.h>

#include <cerrno>
#include <string>

#include <zbi-bootfs/zbi-bootfs.h>
#include <zxtest/zxtest.h>

namespace {

constexpr char kGeneratedZbi[] = "generated.zbi";
constexpr char kGeneratedZbiFilename[] = "payload_1";

constexpr char kZstdZbi[] = "zstd.zbi";
constexpr char kZstdZbiFilename[] = "zstd_1";

constexpr char kLz4fZbi[] = "lz4f.zbi";
constexpr char kLz4fZbiFilename[] = "lz4f_2";

static std::string ImagePath(std::string filename) {
  const char* root_dir = getenv("TEST_ROOT_DIR");
  if (root_dir == nullptr) {
    root_dir = "";
  }
  return std::string(root_dir) + "/testdata/zbi-bootfs/" + filename;
}

static void AssertHasContents(const zbi_bootfs::Entry& entry, const char* contents) {
  auto buffer = std::make_unique<std::byte[]>(entry.size);
  zx_status_t status = entry.vmo.read(buffer.get(), 0, entry.size);

  ASSERT_EQ(ZX_OK, status);
  ASSERT_BYTES_EQ(contents, buffer.get(), strlen(contents));
}

TEST(ZbiBootfsTestCase, InitSuccess) {
  zbi_bootfs::ZbiBootfsParser image;
  size_t byte_offset = 0;
  const std::string input = ImagePath(kGeneratedZbi);

  // Check good input
  zx::vmo vmo_out;
  ASSERT_EQ(ZX_OK, image.Init(input.c_str(), byte_offset));
}

TEST(ZbiBootfsTestCase, InitBadInput) {
  zbi_bootfs::ZbiBootfsParser image;

  // Check bad input
  const char* input = nullptr;
  size_t byte_offset = 0;
  ASSERT_EQ(ZX_ERR_IO, image.Init(input, byte_offset));
}

TEST(ZbiBootfsTestCase, ProcessGeneratedZbi) {
  zbi_bootfs::ZbiBootfsParser image;
  const std::string input = ImagePath(kGeneratedZbi);
  const char* filename = kGeneratedZbiFilename;
  size_t byte_offset = 0;

  zbi_bootfs::Entry entry;

  ASSERT_EQ(ZX_OK, image.Init(input.c_str(), byte_offset));

  // Check bootfs filename
  // This will return a list of Bootfs entires, plus details of "filename" entry
  ASSERT_EQ(ZX_OK, image.ProcessZbi(filename, &entry));

  AssertHasContents(entry, "test 1");
}

TEST(ZbiBootfsTestCase, ProcessZstdZbi) {
  zbi_bootfs::ZbiBootfsParser image;
  const std::string input = ImagePath(kZstdZbi);
  const char* filename = kZstdZbiFilename;
  size_t byte_offset = 0;

  zbi_bootfs::Entry entry;

  ASSERT_EQ(ZX_OK, image.Init(input.c_str(), byte_offset));

  // Check bootfs filename
  // This will return a list of Bootfs entires, plus details of "filename" entry
  ASSERT_EQ(ZX_OK, image.ProcessZbi(filename, &entry));

  AssertHasContents(entry, "test 1\n");
}

TEST(ZbiBootfsTestCase, ProcessLz4fZbi) {
  zbi_bootfs::ZbiBootfsParser image;
  const std::string input = ImagePath(kLz4fZbi);
  const char* filename = kLz4fZbiFilename;
  size_t byte_offset = 0;

  zbi_bootfs::Entry entry;

  ASSERT_EQ(ZX_OK, image.Init(input.c_str(), byte_offset));

  // Check bootfs filename
  // This will return a list of Bootfs entires, plus details of "filename" entry
  ASSERT_EQ(ZX_OK, image.ProcessZbi(filename, &entry));

  AssertHasContents(entry, "test 2\n");
}

TEST(ZbiBootfsTestCase, ProcessBadOffset) {
  zbi_bootfs::ZbiBootfsParser image;
  const std::string input = ImagePath(kGeneratedZbi);
  const char* filename = kGeneratedZbiFilename;
  zbi_bootfs::Entry entry;

  // Check loading zbi with bad offset value and then try processing it
  // This should return an error
  size_t byte_offset = 1;
  ASSERT_EQ(ZX_OK, image.Init(input.c_str(), byte_offset));
  ASSERT_EQ(ZX_ERR_BAD_STATE, image.ProcessZbi(filename, &entry));
}

TEST(ZbiBootfsTestCase, ProcessBadFile) {
  zbi_bootfs::ZbiBootfsParser image;
  const std::string input = ImagePath(kGeneratedZbi);
  size_t byte_offset = 0;
  zbi_bootfs::Entry entry;

  ASSERT_EQ(ZX_OK, image.Init(input.c_str(), byte_offset));
  // Check bad payload filename
  // This will return a list of payload (Bootfs) entires
  const char* filename = "";
  ASSERT_EQ(ZX_ERR_NOT_FOUND, image.ProcessZbi(filename, &entry));
}

}  // namespace
