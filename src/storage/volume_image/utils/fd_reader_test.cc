// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/storage/volume_image/utils/fd_reader.h"

#include <fcntl.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>

#include <fbl/algorithm.h>
#include <fbl/span.h>
#include <fbl/unique_fd.h>
#include <gtest/gtest.h>

namespace storage::volume_image {
namespace {

TEST(FdReaderTest, CreateFromEmptyPathIsError) { ASSERT_TRUE(FdReader::Create("").is_error()); }

TEST(FdReaderTest, CreateFromPathToInexistentFileIsError) {
  ASSERT_TRUE(
      FdReader::Create("myverylongpaththatdoesnotexistbecauseitsimplydoesnot.nonexistingextension")
          .is_error());
}

class TempFile {
 public:
  static fit::result<TempFile, std::string> Create(unsigned int seed) {
    auto get_next_char = [&seed]() {
      int val = rand_r(&seed) % (10 + 26);
      if (val >= 10) {
        return static_cast<char>(val - 10 + 'a');
      }
      return static_cast<char>(val + '0');
    };

    fbl::unique_fd created_file;
    std::string base;
    auto tmp_dir = std::filesystem::temp_directory_path().generic_string();

    do {
      base = tmp_dir;
      base.append("/tmp_");
      std::array<char, 15> random_suffix;
      for (auto& random_char : random_suffix) {
        random_char = get_next_char();
      }
      base.append(random_suffix.data(), random_suffix.size());
      created_file.reset(open(base.c_str(), O_RDONLY));
    } while (created_file.is_valid());

    created_file.reset(open(base.c_str(), O_CREAT | O_RDWR, 0666));
    if (!created_file.is_valid()) {
      std::string error = "Failed to create temporal file at ";
      error.append(base).append(". More specifically: ").append(strerror(errno));
      return fit::error(error);
    }
    return fit::ok(TempFile(base));
  }

  TempFile() = default;
  TempFile(const TempFile&) = delete;
  TempFile(TempFile&&) = default;
  TempFile& operator=(const TempFile&) = delete;
  TempFile& operator=(TempFile&&) = default;
  ~TempFile() { unlink(path_.c_str()); }

  std::string_view path() const { return path_; }

 private:
  explicit TempFile(std::string_view path) : path_(path) {}
  std::string path_;
};

TEST(FdReaderTest, CreateFromExistingFileIsOk) {
  auto temp_file_result = TempFile::Create(testing::UnitTest::GetInstance()->random_seed());
  ASSERT_TRUE(temp_file_result.is_ok()) << temp_file_result.error();
  TempFile file = temp_file_result.take_value();

  auto fd_reader_result = FdReader::Create(file.path());
  ASSERT_TRUE(fd_reader_result.is_ok());

  auto fd_reader = fd_reader_result.take_value();
  EXPECT_EQ(fd_reader.name(), file.path());
}

// Wrapper on top of posix, to guarantee to write all contents to the file or fail.
void Write(int fd, fbl::Span<const char> buffer) {
  uint64_t written_bytes = 0;
  while (written_bytes < buffer.size()) {
    auto return_code = write(fd, buffer.data() + written_bytes, buffer.size() - written_bytes);
    ASSERT_GT(return_code, 0);
    written_bytes += return_code;
  }
  fsync(fd);
}

TEST(FdReaderTest, ReadReturnsCorrectContents) {
  constexpr std::string_view kFileContents = "12345678901234567890abcedf12345";

  auto temp_file_result = TempFile::Create(testing::UnitTest::GetInstance()->random_seed());
  ASSERT_TRUE(temp_file_result.is_ok()) << temp_file_result.error();
  TempFile file = temp_file_result.take_value();

  fbl::unique_fd target_fd(open(file.path().data(), O_RDWR | O_APPEND));
  ASSERT_TRUE(target_fd.is_valid());
  ASSERT_NO_FATAL_FAILURE(
      Write(target_fd.get(), fbl::Span(kFileContents.data(), kFileContents.size())));

  auto fd_reader_or_error = FdReader::Create(file.path());
  ASSERT_TRUE(fd_reader_or_error.is_ok()) << fd_reader_or_error.error();
  auto reader = fd_reader_or_error.take_value();

  std::vector<uint8_t> buffer(kFileContents.size(), 0);
  std::string error = reader.Read(0, buffer);
  ASSERT_TRUE(error.empty()) << error;

  EXPECT_TRUE(memcmp(kFileContents.data(), buffer.data(), kFileContents.size()) == 0);
}

TEST(FdReaderTest, ReadReturnsCorrectContentsAtOffset) {
  constexpr std::string_view kFileContents = "12345678901234567890abcedf12345";
  constexpr uint64_t kOffset = 10;
  static_assert(kOffset < kFileContents.size());
  auto temp_file_result = TempFile::Create(testing::UnitTest::GetInstance()->random_seed());
  ASSERT_TRUE(temp_file_result.is_ok()) << temp_file_result.error();
  TempFile file = temp_file_result.take_value();

  fbl::unique_fd target_fd(open(file.path().data(), O_RDWR | O_APPEND));
  ASSERT_TRUE(target_fd.is_valid());
  ASSERT_NO_FATAL_FAILURE(
      Write(target_fd.get(), fbl::Span(kFileContents.data(), kFileContents.size())));

  auto fd_reader_or_error = FdReader::Create(file.path());
  ASSERT_TRUE(fd_reader_or_error.is_ok()) << fd_reader_or_error.error();
  auto reader = fd_reader_or_error.take_value();

  std::vector<uint8_t> buffer(kFileContents.size() - kOffset, 0);
  std::string error = reader.Read(kOffset, buffer);
  ASSERT_TRUE(error.empty()) << error;

  EXPECT_TRUE(
      memcmp(kFileContents.data() + kOffset, buffer.data(), kFileContents.size() - kOffset) == 0);
}

TEST(FdReaderTest, ReadMultipleTimesReturnsCorrectContentsAtOffset) {
  constexpr std::string_view kFileContents = "12345678901234567890abcedf12345";

  auto temp_file_result = TempFile::Create(testing::UnitTest::GetInstance()->random_seed());
  ASSERT_TRUE(temp_file_result.is_ok()) << temp_file_result.error();
  TempFile file = temp_file_result.take_value();

  fbl::unique_fd target_fd(open(file.path().data(), O_RDWR | O_APPEND));
  ASSERT_TRUE(target_fd.is_valid());
  ASSERT_NO_FATAL_FAILURE(
      Write(target_fd.get(), fbl::Span(kFileContents.data(), kFileContents.size())));

  auto fd_reader_or_error = FdReader::Create(file.path());
  ASSERT_TRUE(fd_reader_or_error.is_ok()) << fd_reader_or_error.error();
  auto reader = fd_reader_or_error.take_value();

  std::vector<uint8_t> buffer(kFileContents.size(), 0);

  // This checks that, for example a different implementation using read instead of pread, would
  // do appropiate seeks before reading.
  for (uint64_t offset = 0; offset < kFileContents.size() - 1; ++offset) {
    std::string error = reader.Read(offset, fbl::Span(buffer.data(), buffer.size() - offset));
    ASSERT_TRUE(error.empty()) << error;

    EXPECT_TRUE(
        memcmp(kFileContents.data() + offset, buffer.data(), kFileContents.size() - offset) == 0);
  }
}

TEST(FdReaderTest, ReadOutOfBoundsIsError) {
  constexpr std::string_view kFileContents = "12345678901234567890abcedf12345";

  auto temp_file_result = TempFile::Create(testing::UnitTest::GetInstance()->random_seed());
  ASSERT_TRUE(temp_file_result.is_ok()) << temp_file_result.error();
  TempFile file = temp_file_result.take_value();

  fbl::unique_fd target_fd(open(file.path().data(), O_RDWR | O_APPEND));
  ASSERT_TRUE(target_fd.is_valid());
  ASSERT_NO_FATAL_FAILURE(
      Write(target_fd.get(), fbl::Span(kFileContents.data(), kFileContents.size())));

  auto fd_reader_or_error = FdReader::Create(file.path());
  ASSERT_TRUE(fd_reader_or_error.is_ok()) << fd_reader_or_error.error();
  auto reader = fd_reader_or_error.take_value();

  std::vector<uint8_t> buffer(kFileContents.size(), 0);

  // Offset out of bounds.
  EXPECT_FALSE(reader.Read(kFileContents.size(), fbl::Span(buffer.data(), 1)).empty());

  // Try to read too much.
  EXPECT_FALSE(reader.Read(1, fbl::Span(buffer.data(), buffer.size())).empty());
}

}  // namespace
}  // namespace storage::volume_image
