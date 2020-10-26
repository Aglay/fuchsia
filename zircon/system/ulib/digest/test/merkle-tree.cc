// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdlib.h>
#include <zircon/assert.h>
#include <zircon/status.h>

#include <algorithm>
#include <memory>
#include <string>

#include <digest/digest.h>
#include <digest/merkle-tree.h>
#include <digest/node-digest.h>
#include <fbl/alloc_checker.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/lib/testing/predicates/status.h"

namespace digest {
namespace {

using ::testing::ElementsAreArray;
using ::testing::Not;
using ::testing::TestParamInfo;
using ::testing::TestWithParam;
using ::testing::ValuesIn;

// The MerkleTree tests below are naturally sensitive to the shape of the Merkle
// tree. These determine those sizes in a consistent way.
const uint64_t kNodeSize = kDefaultNodeSize;
const uint64_t kDigestsPerNode = kNodeSize / kSha256Length;

// The hard-coded trees used for testing were created by using sha256sum on
// files generated using echo -ne, dd, and xxd
struct TreeParam {
  size_t data_len;
  size_t tree_len;
  bool use_compact_format;
  const char digest[(kSha256Length * 2) + 1];
};

constexpr TreeParam kTreeParams[] = {
    {0, 0, false, "15ec7bf0b50732b49f8228e07d24365338f9e3ab994b00af08e5a3bffe55fd8b"},
    {1, 0, false, "0967e0f62a104d1595610d272dfab3d2fa2fe07be0eebce13ef5d79db142610e"},
    {kNodeSize / 2, 0, false, "0a90612c255555469dead72c8fdc41eec06dfe04a30a1f2b7c480ff95d20c5ec"},
    {kNodeSize - 1, 0, false, "f2abd690381bab3ce485c814d05c310b22c34a7441418b5c1a002c344a80e730"},
    {kNodeSize, 0, false, "68d131bc271f9c192d4f6dcd8fe61bef90004856da19d0f2f514a7f4098b0737"},
    {kNodeSize + 1, kNodeSize, false,
     "374781f7d770b6ee9c1a63e186d2d0ccdad10d6aef4fd027e82b1be5b70a2a0c"},
    {kNodeSize * 8, kNodeSize, false,
     "f75f59a944d2433bc6830ec243bfefa457704d2aed12f30539cd4f18bf1d62cf"},
    {kNodeSize * (kDigestsPerNode + 1), kNodeSize * 3, false,
     "7d75dfb18bfd48e03b5be4e8e9aeea2f89880cb81c1551df855e0d0a0cc59a67"},
    {kNodeSize * (kDigestsPerNode + 1) + (kNodeSize / 2), kNodeSize * 3, false,
     "7577266aa98ce587922fdc668c186e27f3c742fb1b732737153b70ae46973e43"},
};

constexpr TreeParam kCompactTreeParams[] = {
    {0, 0, true, "15ec7bf0b50732b49f8228e07d24365338f9e3ab994b00af08e5a3bffe55fd8b"},
    {1, 0, true, "0967e0f62a104d1595610d272dfab3d2fa2fe07be0eebce13ef5d79db142610e"},
    {kNodeSize / 2, 0, true, "0a90612c255555469dead72c8fdc41eec06dfe04a30a1f2b7c480ff95d20c5ec"},
    {kNodeSize - 1, 0, true, "f2abd690381bab3ce485c814d05c310b22c34a7441418b5c1a002c344a80e730"},
    {kNodeSize, 0, true, "68d131bc271f9c192d4f6dcd8fe61bef90004856da19d0f2f514a7f4098b0737"},
    {kNodeSize + 1, kSha256Length * 2, true,
     "374781f7d770b6ee9c1a63e186d2d0ccdad10d6aef4fd027e82b1be5b70a2a0c"},
    {kNodeSize * 8, kSha256Length * 8, true,
     "f75f59a944d2433bc6830ec243bfefa457704d2aed12f30539cd4f18bf1d62cf"},
    {kNodeSize * (kDigestsPerNode + 1), kNodeSize + kSha256Length * 3, true,
     "7d75dfb18bfd48e03b5be4e8e9aeea2f89880cb81c1551df855e0d0a0cc59a67"},
    {kNodeSize * (kDigestsPerNode + 1) + (kNodeSize / 2), kNodeSize + kSha256Length * 4, true,
     "7577266aa98ce587922fdc668c186e27f3c742fb1b732737153b70ae46973e43"},
};

std::unique_ptr<uint8_t[]> AllocateBuffer(size_t len, int value) {
  if (len == 0) {
    return nullptr;
  }
  fbl::AllocChecker ac;
  std::unique_ptr<uint8_t[]> buffer(new (&ac) uint8_t[len]);
  ZX_ASSERT(ac.check());
  memset(buffer.get(), value, len);
  return buffer;
}

class MerkleTreeTest : public TestWithParam<TreeParam> {};

template <class MT>
void TestGetTreeLength(const TreeParam &tree_param) {
  MT mt;
  mt.SetUseCompactFormat(tree_param.use_compact_format);
  EXPECT_OK(mt.SetDataLength(tree_param.data_len));
  EXPECT_EQ(mt.GetTreeLength(), tree_param.tree_len);
}

TEST_P(MerkleTreeTest, MerkleTreeCreatorGetTreeLength) {
  TestGetTreeLength<MerkleTreeCreator>(GetParam());
}

TEST_P(MerkleTreeTest, MerkleTreeVerifierGetTreeLength) {
  TestGetTreeLength<MerkleTreeVerifier>(GetParam());
}

template <class MT>
void TestSetTree(const TreeParam &tree_param) {
  MT mt;
  mt.SetUseCompactFormat(tree_param.use_compact_format);
  uint8_t root[kSha256Length];
  size_t tree_len = tree_param.tree_len;
  std::unique_ptr<uint8_t[]> tree = AllocateBuffer(tree_len, 0x00);
  ASSERT_OK(mt.SetDataLength(tree_param.data_len));
  if (tree_len > 0) {
    EXPECT_STATUS(mt.SetTree(nullptr, tree_len, root, sizeof(root)), ZX_ERR_INVALID_ARGS);
    EXPECT_STATUS(mt.SetTree(tree.get(), tree_len - 1, root, sizeof(root)),
                  ZX_ERR_BUFFER_TOO_SMALL);
  }
  EXPECT_STATUS(mt.SetTree(tree.get(), tree_len, nullptr, sizeof(root)), ZX_ERR_INVALID_ARGS);
  EXPECT_STATUS(mt.SetTree(tree.get(), tree_len, root, sizeof(root) - 1), ZX_ERR_BUFFER_TOO_SMALL);
  EXPECT_OK(mt.SetTree(tree.get(), tree_len, root, sizeof(root)));
}

TEST_P(MerkleTreeTest, MerkleTreeCreatorSetTree) { TestSetTree<MerkleTreeCreator>(GetParam()); }

TEST_P(MerkleTreeTest, MerkleTreeVerifierSetTree) { TestSetTree<MerkleTreeVerifier>(GetParam()); }

TEST_P(MerkleTreeTest, Create) {
  const TreeParam &tree_param = GetParam();
  size_t data_len = tree_param.data_len;
  std::unique_ptr<uint8_t[]> data = AllocateBuffer(data_len, 0xff);
  size_t tree_len = tree_param.tree_len;
  std::unique_ptr<uint8_t[]> tree = AllocateBuffer(tree_len, 0x00);

  Digest digest;
  ASSERT_OK(digest.Parse(tree_param.digest));

  uint8_t root[kSha256Length];
  memset(root, 0, sizeof(root));

  // Valid, added all at once
  MerkleTreeCreator creator;
  creator.SetUseCompactFormat(tree_param.use_compact_format);
  ASSERT_OK(creator.SetDataLength(data_len));
  ASSERT_OK(creator.SetTree(tree.get(), tree_len, root, sizeof(root)));
  EXPECT_OK(creator.Append(data.get(), data_len));
  EXPECT_THAT(root, ElementsAreArray(digest.get(), sizeof(root)));
  // Can reuse creator
  memset(root, 0, sizeof(root));
  ASSERT_OK(creator.SetDataLength(data_len));
  ASSERT_OK(creator.SetTree(tree.get(), tree_len, root, sizeof(root)));
  // Adding zero length has no effect
  EXPECT_OK(creator.Append(nullptr, 0));
  if (data_len != 0) {
    EXPECT_THAT(root, Not(ElementsAreArray(digest.get(), sizeof(root))));
    // Not enough data
    for (size_t j = 0; j < data_len - 1; ++j) {
      EXPECT_OK(creator.Append(&data[j], 1));
    }
    // Valid, added byte by byte
    EXPECT_OK(creator.Append(&data[data_len - 1], 1));
  }
  EXPECT_THAT(root, ElementsAreArray(digest.get(), sizeof(root)));
  // Adding zero length has no effect
  EXPECT_OK(creator.Append(nullptr, 0));
  EXPECT_THAT(root, ElementsAreArray(digest.get(), sizeof(root)));
  // Too much
  EXPECT_STATUS(creator.Append(data.get(), 1), ZX_ERR_INVALID_ARGS);
}

TEST_P(MerkleTreeTest, Verify) {
  srand(::testing::UnitTest::GetInstance()->random_seed());
  const TreeParam &tree_param = GetParam();
  size_t data_len = tree_param.data_len;
  std::unique_ptr<uint8_t[]> data = AllocateBuffer(data_len, 0xff);
  size_t tree_len = tree_param.tree_len;
  std::unique_ptr<uint8_t[]> tree = AllocateBuffer(tree_len, 0x00);

  Digest digest;
  ASSERT_OK(digest.Parse(tree_param.digest));

  uint8_t root[kSha256Length];
  MerkleTreeCreator creator;
  creator.SetUseCompactFormat(tree_param.use_compact_format);
  ASSERT_OK(creator.SetDataLength(data_len));
  ASSERT_OK(creator.SetTree(tree.get(), tree_len, root, sizeof(root)));
  ASSERT_OK(creator.Append(data.get(), data_len));
  // Verify all
  MerkleTreeVerifier verifier;
  verifier.SetUseCompactFormat(tree_param.use_compact_format);
  EXPECT_OK(verifier.SetDataLength(data_len));
  EXPECT_OK(verifier.SetTree(tree.get(), tree_len, root, sizeof(root)));
  EXPECT_OK(verifier.Verify(data.get(), data_len, 0));
  // Empty range is trivially true
  EXPECT_OK(verifier.Verify(nullptr, 0, 0));
  // Flip a byte in the root
  size_t flip = rand() % sizeof(root);
  root[flip] ^= 0xff;
  EXPECT_STATUS(verifier.Verify(data.get(), data_len, 0), ZX_ERR_IO_DATA_INTEGRITY);
  root[flip] ^= 0xff;
  // Flip a byte in the tree
  if (tree_len > 0) {
    flip = rand() % tree_len;
    tree[flip] ^= 0xff;
    EXPECT_STATUS(verifier.Verify(data.get(), data_len, 0), ZX_ERR_IO_DATA_INTEGRITY);
    tree[flip] ^= 0xff;
  }

  for (size_t data_off = 0; data_off < data_len; data_off += kNodeSize) {
    // Unaligned ( +2 doesn't line up with any node boundarys or data ends in the tree params)
    uint8_t *buf = &data[data_off];
    size_t buf_len = std::min(data_len - data_off, kNodeSize);
    EXPECT_STATUS(verifier.Verify(buf, buf_len + 2, data_off), ZX_ERR_INVALID_ARGS);
    // Verify each node
    EXPECT_OK(verifier.Verify(buf, buf_len, data_off));
    // Flip a byte in the root
    flip = rand() % sizeof(root);
    root[flip] ^= 0xff;
    EXPECT_STATUS(verifier.Verify(buf, buf_len, data_off), ZX_ERR_IO_DATA_INTEGRITY);
    root[flip] ^= 0xff;
  }
  // Flip a byte in data; (statically) verify only that node fails
  if (data_len != 0) {
    flip = rand() % data_len;
    data[flip] ^= 0xff;
    size_t data_off = flip;
    size_t buf_len = 1;
    EXPECT_OK(verifier.Align(&data_off, &buf_len));
    uint8_t *buf = &data[data_off];
    const uint8_t *after = buf + buf_len;
    size_t after_off = data_off + buf_len;
    size_t after_len = data_len - after_off;
    EXPECT_OK(verifier.Verify(data.get(), data_off, 0));
    EXPECT_STATUS(verifier.Verify(buf, buf_len, data_off), ZX_ERR_IO_DATA_INTEGRITY);
    EXPECT_OK(verifier.Verify(after, after_len, after_off));
    data[flip] ^= 0xff;
  }
}

TEST_P(MerkleTreeTest, CalculateMerkleTreeSize) {
  const TreeParam &tree_param = GetParam();
  EXPECT_EQ(
      CalculateMerkleTreeSize(tree_param.data_len, kDefaultNodeSize, tree_param.use_compact_format),
      tree_param.tree_len);
}

class MerkleTreeStaticMethodsTest : public TestWithParam<TreeParam> {};

TEST_P(MerkleTreeStaticMethodsTest, Create) {
  const TreeParam &tree_param = GetParam();
  size_t data_len = tree_param.data_len;
  std::unique_ptr<uint8_t[]> data = AllocateBuffer(data_len, 0xff);
  size_t tree_len;
  std::unique_ptr<uint8_t[]> tree;
  Digest root;
  EXPECT_OK(MerkleTreeCreator::Create(data.get(), data_len, &tree, &tree_len, &root));
  EXPECT_EQ(tree_len, tree_param.tree_len);
  EXPECT_EQ(root.ToString(), tree_param.digest);
}

TEST_P(MerkleTreeStaticMethodsTest, Verify) {
  srand(::testing::UnitTest::GetInstance()->random_seed());
  const TreeParam &tree_param = GetParam();
  size_t data_len = tree_param.data_len;
  std::unique_ptr<uint8_t[]> data = AllocateBuffer(data_len, 0xff);
  size_t tree_len;
  std::unique_ptr<uint8_t[]> tree;
  Digest root;
  EXPECT_OK(MerkleTreeCreator::Create(data.get(), data_len, &tree, &tree_len, &root));
  EXPECT_OK(MerkleTreeVerifier::Verify(data.get(), data_len, /*data_off=*/0, data_len, tree.get(),
                                       tree_len, root));
  if (data_len > 0) {
    // Flip some bits in the data.
    size_t flip = rand() % data_len;
    data.get()[flip] ^= 0xff;
    EXPECT_STATUS(MerkleTreeVerifier::Verify(data.get(), data_len, /*data_off=*/0, data_len,
                                             tree.get(), tree_len, root),
                  ZX_ERR_IO_DATA_INTEGRITY);
    data.get()[flip] ^= 0xff;
  }
  if (tree_len > 0) {
    // Flip some bits in the tree.
    size_t flip = rand() % tree_len;
    tree.get()[flip] ^= 0xff;
    EXPECT_STATUS(MerkleTreeVerifier::Verify(data.get(), data_len, /*data_off=*/0, data_len,
                                             tree.get(), tree_len, root),
                  ZX_ERR_IO_DATA_INTEGRITY);
    tree.get()[flip] ^= 0xff;
  }
  // Flip some bits in the root
  uint8_t flipped_root[kSha256Length];
  root.CopyTo(flipped_root);
  flipped_root[rand() % kSha256Length] ^= 0xff;
  Digest flipped_root_digest(flipped_root);
  EXPECT_STATUS(MerkleTreeVerifier::Verify(data.get(), data_len, /*data_off=*/0, data_len,
                                           tree.get(), tree_len, flipped_root_digest),
                ZX_ERR_IO_DATA_INTEGRITY);
}

std::string TreeParamName(const TestParamInfo<TreeParam> &param) {
  return "DataLen" + std::to_string(param.param.data_len);
}

INSTANTIATE_TEST_SUITE_P(MerkleTree, MerkleTreeTest, ValuesIn(kTreeParams), TreeParamName);

INSTANTIATE_TEST_SUITE_P(CompactMerkleTree, MerkleTreeTest, ValuesIn(kCompactTreeParams),
                         TreeParamName);

INSTANTIATE_TEST_SUITE_P(/*no prefix*/, MerkleTreeStaticMethodsTest, ValuesIn(kTreeParams),
                         TreeParamName);

}  // namespace
}  // namespace digest
