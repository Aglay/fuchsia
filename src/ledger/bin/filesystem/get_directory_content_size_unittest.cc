// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ledger/bin/filesystem/get_directory_content_size.h"

#include <gtest/gtest.h>

#include "src/ledger/bin/platform/platform.h"
#include "src/ledger/bin/platform/scoped_tmp_location.h"
#include "src/ledger/lib/files/detached_path.h"

namespace ledger {
namespace {

const std::string kFileContent = "file content";

TEST(GetDirectoryContentSizeTest, GetDirectoryContentSize) {
  std::unique_ptr<Platform> platform = MakePlatform();

  std::unique_ptr<ScopedTmpLocation> tmp_location =
      platform->file_system()->CreateScopedTmpLocation();

  DetachedPath root = tmp_location->path();
  DetachedPath foo = root.SubPath("foo");
  DetachedPath bar = root.SubPath("bar");
  DetachedPath foo_baz = foo.SubPath("baz");

  ASSERT_TRUE(platform->file_system()->CreateDirectory(foo));
  ASSERT_TRUE(platform->file_system()->WriteFile(bar, kFileContent));
  ASSERT_TRUE(platform->file_system()->WriteFile(foo_baz, kFileContent));
  uint64_t directory_size = 0;
  ASSERT_TRUE(GetDirectoryContentSize(platform->file_system(), root, &directory_size));
  ASSERT_EQ(directory_size, 2 * kFileContent.size());
}

}  // namespace
}  // namespace ledger
