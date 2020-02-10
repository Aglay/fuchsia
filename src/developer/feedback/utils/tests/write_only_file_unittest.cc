// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/feedback/utils/write_only_file.h"

#include "src/lib/files/file.h"
#include "src/lib/files/path.h"
#include "src/lib/files/scoped_temp_dir.h"
#include "third_party/googletest/googlemock/include/gmock/gmock.h"
#include "third_party/googletest/googletest/include/gtest/gtest.h"

namespace feedback {
namespace {

class WriteOnlyFileTest : public testing::Test {
 protected:
  void DeleteFile(const std::string& file_path) {
    ASSERT_TRUE(files::DeletePath(file_path, /*recursive=*/true));
  }

  void ReadFileContents(const std::string& file_path, std::string* contents) {
    std::string _contents;
    ASSERT_TRUE(files::ReadFileToString(file_path, &_contents));
    *contents = _contents;
  }

  std::string RootDirectory() { return temp_dir_.path(); }

 private:
  files::ScopedTempDir temp_dir_;
};

TEST_F(WriteOnlyFileTest, Check_CreatesFile) {
  const std::string file_path = files::JoinPath(RootDirectory(), "file.txt");
  WriteOnlyFile file(FileSize::Megabytes(0));
  EXPECT_TRUE(file.Open(file_path));
}

TEST_F(WriteOnlyFileTest, Attempt_WriteToFileWithNoCapacity) {
  const std::string file_path = files::JoinPath(RootDirectory(), "file.txt");
  WriteOnlyFile file(FileSize::Megabytes(0));

  file.Open(file_path);
  EXPECT_EQ(file.Write("test"), (size_t)0u);
}

TEST_F(WriteOnlyFileTest, Check_WriteToFile) {
  const std::string file_path = files::JoinPath(RootDirectory(), "file.txt");
  const FileSize file_capacity(FileSize::Kilobytes(1));

  FileSize expected_bytes_remaining(file_capacity);

  WriteOnlyFile file(file_capacity);
  file.Open(file_path);

  std::string line1("line1\n");
  EXPECT_TRUE(file.Write(line1));
  expected_bytes_remaining -= line1.size();

  std::string line2("line2\n");
  EXPECT_TRUE(file.Write(line2));
  expected_bytes_remaining -= line2.size();

  EXPECT_EQ(file.BytesRemaining(), expected_bytes_remaining.to_bytes());

  std::string file_contents;
  ReadFileContents(file_path, &file_contents);
  EXPECT_EQ(file_contents, line1 + line2);
}

TEST_F(WriteOnlyFileTest, Check_WriteMoreThanCapacity) {
  const std::string file_path = files::JoinPath(RootDirectory(), "file.txt");
  const FileSize file_capacity(FileSize::Bytes(13));

  FileSize expected_bytes_remaining(file_capacity);

  WriteOnlyFile file(file_capacity);
  file.Open(file_path);

  std::string line1("line1\n");
  EXPECT_TRUE(file.Write(line1));
  expected_bytes_remaining -= line1.size();

  std::string line2("line2\n");
  EXPECT_TRUE(file.Write(line2));
  expected_bytes_remaining -= line2.size();

  EXPECT_FALSE(file.Write("line3\n"));

  EXPECT_EQ(file.BytesRemaining(), expected_bytes_remaining.to_bytes());

  std::string file_contents;
  ReadFileContents(file_path, &file_contents);
  EXPECT_EQ(file_contents, line1 + line2);
}

}  // namespace
}  // namespace feedback
