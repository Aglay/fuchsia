// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tools/symbol-index/symbol_index.h"

#include <filesystem>

#include <gtest/gtest.h>

#include "src/lib/files/scoped_temp_dir.h"

namespace symbol_index {

TEST(SymbolIndexTest, AddAndRemove) {
  SymbolIndex symbol_index;
  ASSERT_EQ(symbol_index.entries().size(), 0UL);

  ASSERT_TRUE(symbol_index.Add("/absolute/path/to/symbol", "/some/build_dir"));
  ASSERT_EQ(symbol_index.entries().size(), 1UL);

  ASSERT_FALSE(symbol_index.Add("/absolute/path/../path/to/symbol/"));
  ASSERT_EQ(symbol_index.entries().size(), 1UL);

  ASSERT_TRUE(symbol_index.Remove("/absolute/path/to/symbol/"));
  ASSERT_EQ(symbol_index.entries().size(), 0UL);

  ASSERT_FALSE(symbol_index.Remove("/absolute/path/to/symbol"));
}

TEST(SymbolIndexTest, AddAndRemoveRelatively) {
  SymbolIndex symbol_index;
  ASSERT_EQ(symbol_index.entries().size(), 0UL);

  ASSERT_TRUE(symbol_index.Add("relative/path/to/symbol"));
  ASSERT_EQ(symbol_index.entries().size(), 1UL);
  ASSERT_EQ(symbol_index.entries()[0].symbol_path[0], '/')
      << symbol_index.entries()[0].symbol_path << " should be an absolute path";

  ASSERT_TRUE(symbol_index.Remove("relative/path//./to/symbol"));
  ASSERT_EQ(symbol_index.entries().size(), 0UL);
}

TEST(SymbolIndexTest, Purge) {
  SymbolIndex symbol_index;

  ASSERT_TRUE(symbol_index.Add("/"));
  ASSERT_TRUE(symbol_index.Add("/should/never/exist"));
  ASSERT_EQ(symbol_index.entries().size(), 2UL);

  ASSERT_EQ(symbol_index.Purge().size(), 1UL) << "Should purged 1 entry";
  ASSERT_EQ(symbol_index.entries().size(), 1UL);
}

TEST(SymbolIndexTest, LoadAndSave) {
  files::ScopedTempDir temp_dir;
  std::string temp_file;

  ASSERT_TRUE(temp_dir.NewTempFile(&temp_file));

  SymbolIndex symbol_index(temp_file);
  ASSERT_TRUE(symbol_index.Load().empty()) << "Empty file should not trigger an error";
  ASSERT_TRUE(symbol_index.Add("/absolute/path/to/symbol"));
  ASSERT_EQ(symbol_index.entries().size(), 1UL) << "File location: " << temp_file;
  ASSERT_TRUE(symbol_index.Save().empty());

  SymbolIndex symbol_index2(temp_file);
  ASSERT_TRUE(symbol_index2.Load().empty());
  ASSERT_EQ(symbol_index2.entries().size(), 1UL) << "File location: " << temp_file;
}

}  // namespace symbol_index
