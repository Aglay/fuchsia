// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "filesystem-mounter.h"

#include <lib/zx/channel.h>
#include <zircon/fidl.h>

#include <blobfs/mount.h>
#include <cobalt-client/cpp/in_memory_logger.h>
#include <gtest/gtest.h>

#include "fs-manager.h"
#include "fshost-fs-provider.h"
#include "metrics.h"
#include "src/storage/fshost/block-watcher.h"

namespace devmgr {
namespace {

std::unique_ptr<FsHostMetrics> MakeMetrics() {
  return std::make_unique<FsHostMetrics>(std::make_unique<cobalt_client::Collector>(
      std::make_unique<cobalt_client::InMemoryLogger>()));
}

class FilesystemMounterHarness : public testing::Test {
 public:
  FilesystemMounterHarness()
      : manager_(FshostBootArgs::Create(), MakeMetrics()), watcher_(manager_, FshostOptions()) {}

  void SetUp() override {
    zx::channel dir_request, lifecycle_request;
    ASSERT_EQ(manager_.Initialize(std::move(dir_request), std::move(lifecycle_request), nullptr,
                                  watcher_),
              ZX_OK);
    manager_.WatchExit();
  }

 protected:
  FsManager manager_;

 private:
  BlockWatcher watcher_;
};

using MounterTest = FilesystemMounterHarness;

TEST_F(MounterTest, CreateFilesystemManager) {}

TEST_F(MounterTest, CreateFilesystemMounter) {
  FshostOptions options;
  FilesystemMounter mounter(manager_, options);
}

TEST_F(MounterTest, PkgfsWillNotMountBeforeBlobAndData) {
  FshostOptions options;
  FilesystemMounter mounter(manager_, options);

  ASSERT_FALSE(mounter.BlobMounted());
  ASSERT_FALSE(mounter.DataMounted());
  mounter.TryMountPkgfs();
  EXPECT_FALSE(mounter.PkgfsMounted());
}

enum class FilesystemType {
  kBlobfs,
  kMinfs,
  kFactoryfs,
};

class TestMounter : public FilesystemMounter {
 public:
  template <typename... Args>
  explicit TestMounter(Args&&... args) : FilesystemMounter(std::forward<Args>(args)...) {}

  void ExpectFilesystem(FilesystemType fs) { expected_filesystem_ = fs; }

  zx_status_t LaunchFs(int argc, const char** argv, zx_handle_t* hnd, uint32_t* ids, size_t len,
                       uint32_t fs_flags) final {
    if (argc != 2) {
      return ZX_ERR_INVALID_ARGS;
    }

    switch (expected_filesystem_) {
      case FilesystemType::kBlobfs:
        EXPECT_EQ(std::string_view(argv[0]), "/boot/bin/blobfs");
        EXPECT_EQ(fs_flags, unsigned{FS_SVC | FS_SVC_BLOBFS});
        EXPECT_EQ(len, 3ul);

        // TODO(fxbug.dev/54521): This check is over-constraining.
        // BlobFS does not *require* this handle to be
        // passed in. However, filesystem-mounter will
        // always pass this handle in. Remove this check
        // once we migrate away from using this handle.
        EXPECT_EQ(ids[2], FS_HANDLE_DIAGNOSTICS_DIR);
        break;
      case FilesystemType::kMinfs:
        EXPECT_EQ(std::string_view(argv[0]), "/boot/bin/minfs");
        EXPECT_EQ(fs_flags, unsigned{FS_SVC});
        EXPECT_EQ(len, 2ul);
        break;
      case FilesystemType::kFactoryfs:
        EXPECT_EQ(std::string_view(argv[0]), "/boot/bin/factoryfs");
        EXPECT_EQ(fs_flags, unsigned{FS_SVC});
        break;
      default:
        ADD_FAILURE() << "Unexpected filesystem type";
    }

    EXPECT_EQ(std::string_view(argv[1]), "mount");

    EXPECT_EQ(ids[0], FS_HANDLE_ROOT_ID);
    EXPECT_EQ(ids[1], FS_HANDLE_BLOCK_DEVICE_ID);

    zx::channel* server = nullptr;
    switch (expected_filesystem_) {
      case FilesystemType::kBlobfs:
        server = &blobfs_server_;
        break;
      case FilesystemType::kMinfs:
        server = &minfs_server_;
        break;
      case FilesystemType::kFactoryfs:
        server = &factoryfs_server_;
        break;
      default:
        ADD_FAILURE() << "Unexpected filesystem type";
    }

    server->reset(hnd[0]);
    EXPECT_EQ(server->signal_peer(0, ZX_USER_SIGNAL_0), ZX_OK);
    EXPECT_EQ(zx_handle_close(hnd[1]), ZX_OK);
    return ZX_OK;
  }

 private:
  FilesystemType expected_filesystem_ = FilesystemType::kBlobfs;
  zx::channel blobfs_server_;
  zx::channel minfs_server_;
  zx::channel factoryfs_server_;
};

TEST_F(MounterTest, DurableMount) {
  TestMounter mounter(manager_, FshostOptions());

  mount_options_t options = default_mount_options;
  mounter.ExpectFilesystem(FilesystemType::kMinfs);
  ASSERT_EQ(mounter.MountDurable(zx::channel(), options), ZX_OK);
  ASSERT_TRUE(mounter.DurableMounted());
}

TEST_F(MounterTest, FactoryMount) {
  TestMounter mounter(manager_, FshostOptions());

  mount_options_t options = default_mount_options;
  mounter.ExpectFilesystem(FilesystemType::kFactoryfs);
  ASSERT_EQ(mounter.MountFactoryFs(zx::channel(), options), ZX_OK);

  ASSERT_TRUE(mounter.FactoryMounted());
}

TEST_F(MounterTest, PkgfsWillNotMountBeforeData) {
  FshostOptions fshost_options = {.wait_for_data = true};
  TestMounter mounter(manager_, fshost_options);

  mount_options_t options = default_mount_options;
  mounter.ExpectFilesystem(FilesystemType::kBlobfs);
  ASSERT_EQ(mounter.MountBlob(zx::channel(), options), ZX_OK);

  ASSERT_TRUE(mounter.BlobMounted());
  ASSERT_FALSE(mounter.DataMounted());
  mounter.TryMountPkgfs();
  EXPECT_FALSE(mounter.PkgfsMounted());
}

TEST_F(MounterTest, PkgfsWillNotMountBeforeDataUnlessExplicitlyRequested) {
  FshostOptions fshost_options = {.wait_for_data = false};
  TestMounter mounter(manager_, fshost_options);

  mount_options_t options = default_mount_options;
  mounter.ExpectFilesystem(FilesystemType::kBlobfs);
  ASSERT_EQ(mounter.MountBlob(zx::channel(), options), ZX_OK);

  ASSERT_TRUE(mounter.BlobMounted());
  ASSERT_FALSE(mounter.DataMounted());
  mounter.TryMountPkgfs();
  EXPECT_TRUE(mounter.PkgfsMounted());
}

TEST_F(MounterTest, PkgfsWillNotMountBeforeBlob) {
  FshostOptions fshost_options = {.wait_for_data = true};
  TestMounter mounter(manager_, fshost_options);

  mount_options_t options = default_mount_options;
  mounter.ExpectFilesystem(FilesystemType::kMinfs);
  ASSERT_EQ(mounter.MountData(zx::channel(), options), ZX_OK);

  ASSERT_FALSE(mounter.BlobMounted());
  ASSERT_TRUE(mounter.DataMounted());
  mounter.TryMountPkgfs();
  EXPECT_FALSE(mounter.PkgfsMounted());
}

TEST_F(MounterTest, PkgfsMountsWithBlobAndData) {
  FshostOptions fshost_options = {.wait_for_data = true};
  TestMounter mounter(manager_, fshost_options);

  mount_options_t options = default_mount_options;
  mounter.ExpectFilesystem(FilesystemType::kBlobfs);
  ASSERT_EQ(mounter.MountBlob(zx::channel(), options), ZX_OK);
  mounter.ExpectFilesystem(FilesystemType::kMinfs);
  ASSERT_EQ(mounter.MountData(zx::channel(), options), ZX_OK);

  ASSERT_TRUE(mounter.BlobMounted());
  ASSERT_TRUE(mounter.DataMounted());
  mounter.TryMountPkgfs();
  EXPECT_TRUE(mounter.PkgfsMounted());
}

}  // namespace
}  // namespace devmgr
