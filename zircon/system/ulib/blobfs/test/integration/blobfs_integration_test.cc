// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fuchsia/blobfs/c/fidl.h>
#include <fuchsia/io/c/fidl.h>
#include <lib/fzl/fdio.h>
#include <lib/zx/vmo.h>
#include <sys/mman.h>
#include <utime.h>
#include <zircon/device/vfs.h>

#include <array>
#include <atomic>
#include <thread>
#include <vector>

#include <digest/digest.h>
#include <fbl/auto_call.h>
#include <fvm/format.h>
#include <zxtest/zxtest.h>

#include "blobfs_fixtures.h"
#include "test_support.h"

namespace {

// This is work in progress!. See ZX-4203 for context.

/*
using digest::Digest;
using digest::MerkleTree;

// Helper functions for testing:

static bool MakeBlobUnverified(fs_test_utils::BlobInfo* info, fbl::unique_fd* out_fd) {
    fbl::unique_fd fd(open(info->path, O_CREAT | O_RDWR));
    ASSERT_TRUE(fd, "Failed to create blob");
    ASSERT_EQ(ftruncate(fd.get(), info->size_data), 0);
    ASSERT_EQ(fs_test_utils::StreamAll(write, fd.get(), info->data.get(), info->size_data), 0,
              "Failed to write Data");
    out_fd->reset(fd.release());
    return true;
}

static bool VerifyCompromised(int fd, const char* data, size_t size_data) {
    // Verify the contents of the Blob
    fbl::AllocChecker ac;
    fbl::unique_ptr<char[]> buf(new (&ac) char[size_data]);
    EXPECT_EQ(ac.check(), true);

    ASSERT_EQ(lseek(fd, 0, SEEK_SET), 0);
    ASSERT_EQ(fs_test_utils::StreamAll(read, fd, &buf[0], size_data), -1,
                                       "Expected reading to fail");
    return true;
}

// Creates a blob with the provided Merkle tree + Data, and
// reads to verify the data.
static bool MakeBlobCompromised(fs_test_utils::BlobInfo* info) {
    fbl::unique_fd fd(open(info->path, O_CREAT | O_RDWR));
    ASSERT_TRUE(fd, "Failed to create blob");
    ASSERT_EQ(ftruncate(fd.get(), info->size_data), 0);

    // If we're writing a blob with invalid sizes, it's possible that writing will fail.
    fs_test_utils::StreamAll(write, fd.get(), info->data.get(), info->size_data);

    ASSERT_TRUE(VerifyCompromised(fd.get(), info->data.get(), info->size_data));
    ASSERT_EQ(close(fd.release()), 0);
    return true;
}

static bool uint8_to_hex_str(const uint8_t* data, char* hex_str) {
    for (size_t i = 0; i < 32; i++) {
        ASSERT_EQ(sprintf(hex_str + (i * 2), "%02x", data[i]), 2,
                  "Error converting name to string");
    }
    hex_str[64] = 0;
    return true;
}

*/

// Go over the parent device logic and test fixture.
TEST_F(BlobfsTest, Trivial) {}

TEST_F(BlobfsTestWithFvm, Trivial) {}

void RunBasicsTest() {
  for (unsigned int i = 10; i < 16; i++) {
    std::unique_ptr<fs_test_utils::BlobInfo> info;
    ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(kMountPath, 1 << i, &info));

    fbl::unique_fd fd;
    ASSERT_NO_FAILURES(MakeBlob(info.get(), &fd));
    ASSERT_EQ(close(fd.release()), 0);

    // We can re-open and verify the Blob as read-only
    fd.reset(open(info->path, O_RDONLY));
    ASSERT_TRUE(fd, "Failed to-reopen blob");
    ASSERT_TRUE(fs_test_utils::VerifyContents(fd.get(), info->data.get(), info->size_data));
    ASSERT_EQ(close(fd.release()), 0);

    // We cannot re-open the blob as writable
    fd.reset(open(info->path, O_RDWR | O_CREAT));
    ASSERT_FALSE(fd, "Shouldn't be able to re-create blob that exists");
    fd.reset(open(info->path, O_RDWR));
    ASSERT_FALSE(fd, "Shouldn't be able to re-open blob as writable");
    fd.reset(open(info->path, O_WRONLY));
    ASSERT_FALSE(fd, "Shouldn't be able to re-open blob as writable");

    ASSERT_EQ(unlink(info->path), 0);
  }
}

TEST_F(BlobfsTest, Basics) { RunBasicsTest(); }

TEST_F(BlobfsTestWithFvm, Basics) { RunBasicsTest(); }

void RunUnallocatedBlobTest() {
  std::unique_ptr<fs_test_utils::BlobInfo> info;
  ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(kMountPath, 1 << 10, &info));

  // We can create a blob with a name.
  ASSERT_TRUE(fbl::unique_fd(open(info->path, O_CREAT | O_EXCL | O_RDWR)));
  // It won't exist if we close it before allocating space.
  ASSERT_FALSE(fbl::unique_fd(open(info->path, O_RDWR)));
  ASSERT_FALSE(fbl::unique_fd(open(info->path, O_RDONLY)));
  // We can "re-use" the name.
  {
    fbl::unique_fd fd(open(info->path, O_CREAT | O_EXCL | O_RDWR));
    ASSERT_TRUE(fd);
    ASSERT_EQ(ftruncate(fd.get(), info->size_data), 0);
  }
}

TEST_F(BlobfsTest, UnallocatedBlob) { RunUnallocatedBlobTest(); }

TEST_F(BlobfsTestWithFvm, UnallocatedBlob) { RunUnallocatedBlobTest(); }

void RunNullBlobCreateUnlinkTest() {
  std::unique_ptr<fs_test_utils::BlobInfo> info;
  ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(kMountPath, 0, &info));

  fbl::unique_fd fd(open(info->path, O_CREAT | O_EXCL | O_RDWR));
  ASSERT_TRUE(fd);
  ASSERT_EQ(ftruncate(fd.get(), 0), 0);
  std::array<char, 1> buf;
  ASSERT_EQ(read(fd.get(), &buf[0], 1), 0, "Null Blob should reach EOF immediately");
  ASSERT_EQ(close(fd.release()), 0);

  fd.reset(open(info->path, O_CREAT | O_EXCL | O_RDWR));
  EXPECT_FALSE(fd, "Null Blob should already exist");
  fd.reset(open(info->path, O_CREAT | O_RDWR));
  EXPECT_FALSE(fd, "Null Blob should not be openable as writable");

  fd.reset(open(info->path, O_RDONLY));
  ASSERT_TRUE(fd, "Null blob should be re-openable");

  DIR* dir = opendir(kMountPath);
  ASSERT_NOT_NULL(dir);
  auto cleanup = fbl::MakeAutoCall([dir]() { closedir(dir); });
  struct dirent* entry = readdir(dir);
  ASSERT_NOT_NULL(entry);
  const char* kEmptyBlobName = "15ec7bf0b50732b49f8228e07d24365338f9e3ab994b00af08e5a3bffe55fd8b";
  EXPECT_STR_EQ(kEmptyBlobName, entry->d_name, "Unexpected name from readdir");
  EXPECT_NULL(readdir(dir));

  ASSERT_EQ(close(fd.release()), 0);
  ASSERT_EQ(unlink(info->path), 0, "Null Blob should be unlinkable");
}

TEST_F(BlobfsTest, NullBlobCreateUnlink) { RunNullBlobCreateUnlinkTest(); }

TEST_F(BlobfsTestWithFvm, NullBlobCreateUnlink) { RunNullBlobCreateUnlinkTest(); }

void RunNullBlobCreateRemountTest(FilesystemTest* test) {
  std::unique_ptr<fs_test_utils::BlobInfo> info;
  ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(kMountPath, 0, &info));

  // Create the null blob.
  fbl::unique_fd fd(open(info->path, O_CREAT | O_EXCL | O_RDWR));
  ASSERT_TRUE(fd);
  ASSERT_EQ(ftruncate(fd.get(), 0), 0);
  ASSERT_EQ(close(fd.release()), 0);

  ASSERT_NO_FAILURES(test->Remount());
  fd.reset(open(info->path, O_RDONLY));
  ASSERT_TRUE(fd, "Null blob lost after reboot");
  ASSERT_EQ(close(fd.release()), 0);
  ASSERT_EQ(unlink(info->path), 0, "Null Blob should be unlinkable");
}

TEST_F(BlobfsTest, NullBlobCreateRemount) { RunNullBlobCreateRemountTest(this); }

TEST_F(BlobfsTestWithFvm, NullBlobCreateRemount) { RunNullBlobCreateRemountTest(this); }

void RunExclusiveCreateTest() {
  std::unique_ptr<fs_test_utils::BlobInfo> info;

  ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(kMountPath, 1 << 17, &info));
  fbl::unique_fd fd(open(info->path, O_CREAT | O_EXCL | O_RDWR));
  ASSERT_TRUE(fd);

  fbl::unique_fd fd2(open(info->path, O_CREAT | O_EXCL | O_RDWR));
  EXPECT_FALSE(fd2, "Should not be able to exclusively create twice");

  // But a second open should work.
  fd2.reset(open(info->path, O_CREAT | O_RDWR));
  ASSERT_TRUE(fd2);
}

TEST_F(BlobfsTest, ExclusiveCreate) { RunExclusiveCreateTest(); }

TEST_F(BlobfsTestWithFvm, ExclusiveCreate) { RunExclusiveCreateTest(); }

void RunCompressibleBlobTest(FilesystemTest* test) {
  for (size_t i = 10; i < 22; i++) {
    std::unique_ptr<fs_test_utils::BlobInfo> info;

    // Create blobs which are trivially compressible.
    ASSERT_TRUE(fs_test_utils::GenerateBlob(
        [](char* data, size_t length) {
          size_t i = 0;
          while (i < length) {
            size_t j = (rand() % (length - i)) + 1;
            memset(data, (char)j, j);
            data += j;
            i += j;
          }
        },
        kMountPath, 1 << i, &info));

    fbl::unique_fd fd;
    ASSERT_NO_FAILURES(MakeBlob(info.get(), &fd));

    // We can re-open and verify the Blob as read-only.
    fd.reset(open(info->path, O_RDONLY));
    ASSERT_TRUE(fd, "Failed to-reopen blob");
    ASSERT_TRUE(fs_test_utils::VerifyContents(fd.get(), info->data.get(), info->size_data));

    // Force decompression by remounting, re-accessing blob.
    ASSERT_NO_FAILURES(test->Remount());
    fd.reset(open(info->path, O_RDONLY));
    ASSERT_TRUE(fd, "Failed to-reopen blob");
    ASSERT_TRUE(fs_test_utils::VerifyContents(fd.get(), info->data.get(), info->size_data));

    ASSERT_EQ(0, unlink(info->path));
  }
}

TEST_F(BlobfsTest, CompressibleBlob) { RunCompressibleBlobTest(this); }

TEST_F(BlobfsTestWithFvm, CompressibleBlob) { RunCompressibleBlobTest(this); }

void RunMmapTest() {
  for (size_t i = 10; i < 16; i++) {
    std::unique_ptr<fs_test_utils::BlobInfo> info;
    ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(kMountPath, 1 << i, &info));

    fbl::unique_fd fd;
    ASSERT_NO_FAILURES(MakeBlob(info.get(), &fd));
    fd.reset(open(info->path, O_RDONLY));
    ASSERT_TRUE(fd, "Failed to-reopen blob");

    void* addr = mmap(NULL, info->size_data, PROT_READ, MAP_PRIVATE, fd.get(), 0);
    ASSERT_NE(addr, MAP_FAILED, "Could not mmap blob");
    ASSERT_BYTES_EQ(addr, info->data.get(), info->size_data);
    ASSERT_EQ(0, munmap(addr, info->size_data));
    ASSERT_EQ(0, unlink(info->path));
  }
}

TEST_F(BlobfsTest, Mmap) { RunMmapTest(); }

TEST_F(BlobfsTestWithFvm, Mmap) { RunMmapTest(); }

void RunMmapUseAfterCloseTest() {
  for (size_t i = 10; i < 16; i++) {
    std::unique_ptr<fs_test_utils::BlobInfo> info;
    ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(kMountPath, 1 << i, &info));

    fbl::unique_fd fd;
    ASSERT_NO_FAILURES(MakeBlob(info.get(), &fd));
    fd.reset(open(info->path, O_RDONLY));
    ASSERT_TRUE(fd, "Failed to-reopen blob");

    void* addr = mmap(NULL, info->size_data, PROT_READ, MAP_PRIVATE, fd.get(), 0);
    ASSERT_NE(addr, MAP_FAILED, "Could not mmap blob");
    fd.reset();

    // We should be able to access the mapped data after the file is closed.
    ASSERT_BYTES_EQ(addr, info->data.get(), info->size_data);

    // We should be able to re-open and remap the file.
    //
    // Although this isn't being tested explicitly (we lack a mechanism to
    // check that the second mapping uses the same underlying pages as the
    // first) the memory usage should avoid duplication in the second
    // mapping.
    fd.reset(open(info->path, O_RDONLY));
    void* addr2 = mmap(NULL, info->size_data, PROT_READ, MAP_PRIVATE, fd.get(), 0);
    ASSERT_NE(addr2, MAP_FAILED, "Could not mmap blob");
    fd.reset();
    ASSERT_BYTES_EQ(addr2, info->data.get(), info->size_data);

    ASSERT_EQ(munmap(addr, info->size_data), 0, "Could not unmap blob");
    ASSERT_EQ(munmap(addr2, info->size_data), 0, "Could not unmap blob");

    ASSERT_EQ(0, unlink(info->path));
  }
}

TEST_F(BlobfsTest, MmapUseAfterClose) { RunMmapUseAfterCloseTest(); }

TEST_F(BlobfsTestWithFvm, MmapUseAfterClose) { RunMmapUseAfterCloseTest(); }

void RunReadDirectoryTest() {
  constexpr size_t kMaxEntries = 50;
  constexpr size_t kBlobSize = 1 << 10;

  std::unique_ptr<fs_test_utils::BlobInfo> info[kMaxEntries];

  // Try to readdir on an empty directory.
  DIR* dir = opendir(kMountPath);
  ASSERT_NOT_NULL(dir);
  auto cleanup = fbl::MakeAutoCall([dir]() { closedir(dir); });
  ASSERT_NULL(readdir(dir), "Expected blobfs to start empty");

  // Fill a directory with entries.
  for (size_t i = 0; i < kMaxEntries; i++) {
    ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(kMountPath, kBlobSize, &info[i]));
    fbl::unique_fd fd;
    ASSERT_NO_FAILURES(MakeBlob(info[i].get(), &fd));
  }

  // Check that we see the expected number of entries
  size_t entries_seen = 0;
  struct dirent* dir_entry;
  while ((dir_entry = readdir(dir)) != nullptr) {
    entries_seen++;
  }
  ASSERT_EQ(kMaxEntries, entries_seen);
  entries_seen = 0;
  rewinddir(dir);

  // Readdir on a directory which contains entries, removing them as we go
  // along.
  while ((dir_entry = readdir(dir)) != nullptr) {
    bool found = false;
    for (size_t i = 0; i < kMaxEntries; i++) {
      if ((info[i]->size_data != 0) &&
          strcmp(strrchr(info[i]->path, '/') + 1, dir_entry->d_name) == 0) {
        ASSERT_EQ(0, unlink(info[i]->path));
        // It's a bit hacky, but we set 'size_data' to zero
        // to identify the entry has been unlinked.
        info[i]->size_data = 0;
        found = true;
        break;
      }
    }
    ASSERT_TRUE(found, "Unknown directory entry");
    entries_seen++;
  }
  ASSERT_EQ(kMaxEntries, entries_seen);

  ASSERT_NULL(readdir(dir), "Directory should be empty");
  cleanup.cancel();
  ASSERT_EQ(0, closedir(dir));
}

TEST_F(BlobfsTest, ReadDirectory) { RunReadDirectoryTest(); }

TEST_F(BlobfsTestWithFvm, ReadDirectory) { RunReadDirectoryTest(); }

class SmallDiskTest : public BlobfsFixedDiskSizeTest {
 public:
  SmallDiskTest() : BlobfsFixedDiskSizeTest(MinimumDiskSize()) {}
  explicit SmallDiskTest(uint64_t disk_size) : BlobfsFixedDiskSizeTest(disk_size) {}

 protected:
  static uint64_t MinimumDiskSize() {
    blobfs::Superblock info;
    info.inode_count = blobfs::kBlobfsDefaultInodeCount;
    info.data_block_count = blobfs::kMinimumDataBlocks;
    info.journal_block_count = blobfs::kMinimumJournalBlocks;
    info.flags = 0;

    return blobfs::TotalBlocks(info) * blobfs::kBlobfsBlockSize;
  }
};

TEST_F(SmallDiskTest, SmallestValidDisk) {}

class TooSmallDiskTest : public SmallDiskTest {
 public:
  TooSmallDiskTest() : SmallDiskTest(MinimumDiskSize() - 1024) {}

  void SetUp() override {}
  void TearDown() override {}
};

TEST_F(TooSmallDiskTest, DiskTooSmall) {
  ASSERT_NOT_OK(
      mkfs(device_path().c_str(), format_type(), launch_stdio_sync, &default_mkfs_options));
}

class SmallDiskTestWithFvm : public BlobfsFixedDiskSizeTestWithFvm {
 public:
  SmallDiskTestWithFvm() : BlobfsFixedDiskSizeTestWithFvm(MinimumDiskSize()) {}
  explicit SmallDiskTestWithFvm(uint64_t disk_size) : BlobfsFixedDiskSizeTestWithFvm(disk_size) {}

 protected:
  static uint64_t MinimumDiskSize() {
    size_t blocks_per_slice = kTestFvmSliceSize / blobfs::kBlobfsBlockSize;

    // Calculate slices required for data blocks based on minimum requirement and slice size.
    uint64_t required_data_slices =
        fbl::round_up(blobfs::kMinimumDataBlocks, blocks_per_slice) / blocks_per_slice;
    uint64_t required_journal_slices =
        fbl::round_up(blobfs::kDefaultJournalBlocks, blocks_per_slice) / blocks_per_slice;

    // Require an additional 1 slice each for super, inode, and block bitmaps.
    uint64_t blobfs_size = (required_journal_slices + required_data_slices + 3) * kTestFvmSliceSize;
    uint64_t minimum_size = blobfs_size;
    uint64_t metadata_size = fvm::MetadataSize(blobfs_size, kTestFvmSliceSize);

    // Re-calculate minimum size until the metadata size stops growing.
    while (minimum_size - blobfs_size != metadata_size * 2) {
      minimum_size = blobfs_size + metadata_size * 2;
      metadata_size = fvm::MetadataSize(minimum_size, kTestFvmSliceSize);
    }

    return minimum_size;
  }
};

TEST_F(SmallDiskTestWithFvm, SmallestValidDisk) {}

class TooSmallDiskTestWithFvm : public SmallDiskTestWithFvm {
 public:
  TooSmallDiskTestWithFvm() : SmallDiskTestWithFvm(MinimumDiskSize() - 1024) {}

  void SetUp() override { ASSERT_NO_FAILURES(FvmSetUp()); }
  void TearDown() override {}
};

TEST_F(TooSmallDiskTestWithFvm, DiskTooSmall) {
  ASSERT_NOT_OK(
      mkfs(device_path().c_str(), format_type(), launch_stdio_sync, &default_mkfs_options));
}

void QueryInfo(size_t expected_nodes, size_t expected_bytes) {
  fbl::unique_fd fd(open(kMountPath, O_RDONLY | O_DIRECTORY));
  ASSERT_TRUE(fd);

  zx_status_t status;
  fuchsia_io_FilesystemInfo info;
  fzl::FdioCaller caller(std::move(fd));
  ASSERT_OK(fuchsia_io_DirectoryAdminQueryFilesystem(caller.borrow_channel(), &status, &info));
  ASSERT_OK(status);

  const char kFsName[] = "blobfs";
  const char* name = reinterpret_cast<const char*>(info.name);
  ASSERT_STR_EQ(kFsName, name, "Unexpected filesystem mounted");
  EXPECT_EQ(info.block_size, blobfs::kBlobfsBlockSize);
  EXPECT_EQ(info.max_filename_size, digest::Digest::kLength * 2);
  EXPECT_EQ(info.fs_type, VFS_TYPE_BLOBFS);
  EXPECT_NE(info.fs_id, 0);

  // Check that used_bytes are within a reasonable range
  EXPECT_GE(info.used_bytes, expected_bytes);
  EXPECT_LE(info.used_bytes, info.total_bytes);

  // Check that total_bytes are a multiple of slice_size
  EXPECT_GE(info.total_bytes, kTestFvmSliceSize);
  EXPECT_EQ(info.total_bytes % kTestFvmSliceSize, 0);
  EXPECT_EQ(info.total_nodes, kTestFvmSliceSize / blobfs::kBlobfsInodeSize);
  EXPECT_EQ(info.used_nodes, expected_nodes);
}

TEST_F(BlobfsTestWithFvm, QueryInfo) {
  size_t total_bytes = 0;
  ASSERT_NO_FAILURES(QueryInfo(0, 0));
  for (size_t i = 10; i < 16; i++) {
    std::unique_ptr<fs_test_utils::BlobInfo> info;
    ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(kMountPath, 1 << i, &info));

    fbl::unique_fd fd;
    ASSERT_NO_FAILURES(MakeBlob(info.get(), &fd));
    total_bytes += fbl::round_up(info->size_merkle + info->size_data, blobfs::kBlobfsBlockSize);
  }

  ASSERT_NO_FAILURES(QueryInfo(6, total_bytes));
}

void GetAllocations(zx::vmo* out_vmo, uint64_t* out_count) {
  fbl::unique_fd fd(open(kMountPath, O_RDONLY | O_DIRECTORY));
  ASSERT_TRUE(fd);
  zx_status_t status;
  zx_handle_t vmo_handle;
  fzl::FdioCaller caller(std::move(fd));
  ASSERT_OK(fuchsia_blobfs_BlobfsGetAllocatedRegions(caller.borrow_channel(), &status, &vmo_handle,
                                                     out_count));
  ASSERT_OK(status);
  out_vmo->reset(vmo_handle);
}

void RunGetAllocatedRegionsTest() {
  zx::vmo vmo;
  uint64_t count;
  size_t total_bytes = 0;
  size_t fidl_bytes = 0;

  // Although we expect this partition to be empty, we check the results of GetAllocations
  // in case blobfs chooses to store any metadata of pre-initialized data with the
  // allocated regions.
  ASSERT_NO_FAILURES(GetAllocations(&vmo, &count));

  std::vector<fuchsia_blobfs_BlockRegion> buffer(count);
  ASSERT_OK(vmo.read(buffer.data(), 0, sizeof(fuchsia_blobfs_BlockRegion) * count));
  for (size_t i = 0; i < count; i++) {
    total_bytes += buffer[i].length * blobfs::kBlobfsBlockSize;
  }

  for (size_t i = 10; i < 16; i++) {
    std::unique_ptr<fs_test_utils::BlobInfo> info;
    ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(kMountPath, 1 << i, &info));

    fbl::unique_fd fd;
    ASSERT_NO_FAILURES(MakeBlob(info.get(), &fd));
    total_bytes += fbl::round_up(info->size_merkle + info->size_data, blobfs::kBlobfsBlockSize);
  }
  ASSERT_NO_FAILURES(GetAllocations(&vmo, &count));

  buffer.resize(count);
  ASSERT_OK(vmo.read(buffer.data(), 0, sizeof(fuchsia_blobfs_BlockRegion) * count));
  for (size_t i = 0; i < count; i++) {
    fidl_bytes += buffer[i].length * blobfs::kBlobfsBlockSize;
  }
  ASSERT_EQ(fidl_bytes, total_bytes);
}

TEST_F(BlobfsTest, GetAllocatedRegions) { RunGetAllocatedRegionsTest(); }

TEST_F(BlobfsTestWithFvm, GetAllocatedRegions) { RunGetAllocatedRegionsTest(); }

void RunUseAfterUnlinkTest() {
  for (size_t i = 0; i < 16; i++) {
    std::unique_ptr<fs_test_utils::BlobInfo> info;
    ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(kMountPath, 1 << i, &info));

    fbl::unique_fd fd;
    ASSERT_NO_FAILURES(MakeBlob(info.get(), &fd));

    // We should be able to unlink the blob.
    ASSERT_EQ(0, unlink(info->path));

    // We should still be able to read the blob after unlinking.
    ASSERT_TRUE(fs_test_utils::VerifyContents(fd.get(), info->data.get(), info->size_data));

    // After closing the file, however, we should not be able to re-open the blob.
    fd.reset();
    ASSERT_LT(open(info->path, O_RDONLY), 0, "Expected blob to be deleted");
  }
}

TEST_F(BlobfsTest, UseAfterUnlink) { RunUseAfterUnlinkTest(); }

TEST_F(BlobfsTestWithFvm, UseAfterUnlink) { RunUseAfterUnlinkTest(); }

void RunWriteAfterReadtest() {
  srand(zxtest::Runner::GetInstance()->random_seed());
  for (size_t i = 0; i < 16; i++) {
    std::unique_ptr<fs_test_utils::BlobInfo> info;
    ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(kMountPath, 1 << i, &info));

    fbl::unique_fd fd;
    ASSERT_NO_FAILURES(MakeBlob(info.get(), &fd));

    // After blob generation, writes should be rejected.
    ASSERT_LT(write(fd.get(), info->data.get(), 1), 0,
              "After being written, the blob should refuse writes");

    off_t seek_pos = (rand() % info->size_data);
    ASSERT_EQ(seek_pos, lseek(fd.get(), seek_pos, SEEK_SET));
    ASSERT_LT(write(fd.get(), info->data.get(), 1), 0,
              "After being written, the blob should refuse writes");
    ASSERT_LT(ftruncate(fd.get(), rand() % info->size_data), 0,
              "The blob should always refuse to be truncated");

    ASSERT_EQ(0, unlink(info->path));
  }
}

TEST_F(BlobfsTest, WriteAfterRead) { RunWriteAfterReadtest(); }

TEST_F(BlobfsTestWithFvm, WriteAfterRead) { RunWriteAfterReadtest(); }

void RunWriteAfterUnlinkTest() {
  std::unique_ptr<fs_test_utils::BlobInfo> info;
  size_t size = 1 << 20;
  ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(kMountPath, size, &info));

  // Partially write out first blob.
  fbl::unique_fd fd(open(info->path, O_CREAT | O_RDWR));
  ASSERT_TRUE(fd, "Failed to create blob");
  ASSERT_EQ(0, ftruncate(fd.get(), size));
  ASSERT_EQ(0, fs_test_utils::StreamAll(write, fd.get(), info->data.get(), size / 2),
            "Failed to write Data");
  ASSERT_EQ(0, unlink(info->path));
  ASSERT_EQ(
      0, fs_test_utils::StreamAll(write, fd.get(), info->data.get() + size / 2, size - (size / 2)),
      "Failed to write Data");
  fd.reset();
  ASSERT_LT(open(info->path, O_RDONLY), 0);
}

TEST_F(BlobfsTest, WriteAfterUnlink) { RunWriteAfterUnlinkTest(); }

TEST_F(BlobfsTestWithFvm, WriteAfterUnlink) { RunWriteAfterUnlinkTest(); }

void RunReadTooLargeTest() {
  for (size_t i = 0; i < 16; i++) {
    std::unique_ptr<fs_test_utils::BlobInfo> info;
    ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(kMountPath, 1 << i, &info));

    fbl::unique_fd fd;
    ASSERT_NO_FAILURES(MakeBlob(info.get(), &fd));

    std::unique_ptr<char[]> buffer(new char[info->size_data]);

    // Try read beyond end of blob.
    off_t end_off = info->size_data;
    ASSERT_EQ(end_off, lseek(fd.get(), end_off, SEEK_SET));
    ASSERT_EQ(0, read(fd.get(), &buffer[0], 1), "Expected empty read beyond end of file");

    // Try some reads which straddle the end of the blob.
    for (ssize_t j = 1; j < static_cast<ssize_t>(info->size_data); j *= 2) {
      end_off = info->size_data - j;
      ASSERT_EQ(end_off, lseek(fd.get(), end_off, SEEK_SET));
      ASSERT_EQ(j, read(fd.get(), &buffer[0], j * 2),
                "Expected to only read one byte at end of file");
      ASSERT_BYTES_EQ(buffer.get(), &info->data[info->size_data - j], j,
                      "Read data, but it was bad");
    }

    ASSERT_EQ(0, unlink(info->path));
  }
}

TEST_F(BlobfsTest, ReadTooLarge) { RunReadTooLargeTest(); }

TEST_F(BlobfsTestWithFvm, ReadTooLarge) { RunReadTooLargeTest(); }

void RunBadAllocationTest(uint64_t disk_size) {
  std::string name(kMountPath);
  name.append("/00112233445566778899AABBCCDDEEFFGGHHIIJJKKLLMMNNOOPPQQRRSSTTUUVV");
  fbl::unique_fd fd(open(name.c_str(), O_CREAT | O_RDWR));
  ASSERT_FALSE(fd, "Only acceptable pathnames are hex");

  name.assign(kMountPath);
  name.append("/00112233445566778899AABBCCDDEEFF");
  fd.reset(open(name.c_str(), O_CREAT | O_RDWR));
  ASSERT_FALSE(fd, "Only acceptable pathnames are 32 hex-encoded bytes");

  std::unique_ptr<fs_test_utils::BlobInfo> info;
  ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(kMountPath, 1 << 15, &info));

  fd.reset(open(info->path, O_CREAT | O_RDWR));
  ASSERT_TRUE(fd, "Failed to create blob");
  ASSERT_EQ(-1, ftruncate(fd.get(), 0), "Blob without data doesn't match null blob");

  // This is the size of the entire disk; we won't have room.
  ASSERT_EQ(-1, ftruncate(fd.get(), disk_size), "Huge blob");

  // Okay, finally, a valid blob!
  ASSERT_EQ(0, ftruncate(fd.get(), info->size_data), "Failed to allocate blob");

  // Write nothing, but close the blob. Since the write was incomplete,
  // it will be inaccessible.
  fd.reset(open(info->path, O_RDWR));
  ASSERT_FALSE(fd, "Cannot access partial blob");
  fd.reset(open(info->path, O_RDONLY));
  ASSERT_FALSE(fd, "Cannot access partial blob");

  // And once more -- let's write everything but the last byte of a blob's data.
  fd.reset(open(info->path, O_CREAT | O_RDWR));
  ASSERT_TRUE(fd, "Failed to create blob");
  ASSERT_EQ(0, ftruncate(fd.get(), info->size_data), "Failed to allocate blob");
  ASSERT_EQ(0, fs_test_utils::StreamAll(write, fd.get(), info->data.get(), info->size_data - 1),
            "Failed to write data");
  fd.reset(open(info->path, O_RDWR));
  ASSERT_FALSE(fd, "Cannot access partial blob");
}

TEST_F(BlobfsTest, BadAllocation) { RunBadAllocationTest(environment_->disk_size()); }

TEST_F(BlobfsTestWithFvm, BadAllocation) { RunBadAllocationTest(environment_->disk_size()); }

/*

static bool CorruptedBlob(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;

    fbl::unique_ptr<fs_test_utils::BlobInfo> info;
    for (size_t i = 1; i < 18; i++) {
        ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, 1 << i, &info));
        info->size_data -= (rand() % info->size_data) + 1;
        if (info->size_data == 0) {
            info->size_data = 1;
        }
        ASSERT_TRUE(MakeBlobCompromised(info.get()));
    }

    for (size_t i = 0; i < 18; i++) {
        ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, 1 << i, &info));
        // Flip a random bit of the data
        size_t rand_index = rand() % info->size_data;
        char old_val = info->data.get()[rand_index];
        while ((info->data.get()[rand_index] = static_cast<char>(rand())) == old_val) {
        }
        ASSERT_TRUE(MakeBlobCompromised(info.get()));
    }

    END_HELPER;
}

static bool CorruptedDigest(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;

    fbl::unique_ptr<fs_test_utils::BlobInfo> info;
    for (size_t i = 1; i < 18; i++) {
        ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, 1 << i, &info));

        char hexdigits[17] = "0123456789abcdef";
        size_t idx = strlen(info->path) - 1 - (rand() % (2 * Digest::kLength));
        char newchar = hexdigits[rand() % 16];
        while (info->path[idx] == newchar) {
            newchar = hexdigits[rand() % 16];
        }
        info->path[idx] = newchar;
        ASSERT_TRUE(MakeBlobCompromised(info.get()));
    }

    for (size_t i = 0; i < 18; i++) {
        ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, 1 << i, &info));
        // Flip a random bit of the data
        size_t rand_index = rand() % info->size_data;
        char old_val = info->data.get()[rand_index];
        while ((info->data.get()[rand_index] = static_cast<char>(rand())) == old_val) {
        }
        ASSERT_TRUE(MakeBlobCompromised(info.get()));
    }

    END_HELPER;
}

*/

void RunEdgeAllocationTest() {
  // Powers of two...
  for (size_t i = 1; i < 16; i++) {
    // -1, 0, +1 offsets...
    for (size_t j = -1; j < 2; j++) {
      std::unique_ptr<fs_test_utils::BlobInfo> info;
      ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(kMountPath, (1 << i) + j, &info));
      fbl::unique_fd fd;
      ASSERT_NO_FAILURES(MakeBlob(info.get(), &fd));
      ASSERT_EQ(0, unlink(info->path));
    }
  }
}

TEST_F(BlobfsTest, EdgeAllocation) { RunEdgeAllocationTest(); }

TEST_F(BlobfsTestWithFvm, EdgeAllocation) { RunEdgeAllocationTest(); }

void RunUmountWithOpenFileTest(FilesystemTest* test) {
  std::unique_ptr<fs_test_utils::BlobInfo> info;
  ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(kMountPath, 1 << 16, &info));
  fbl::unique_fd fd;
  ASSERT_NO_FAILURES(MakeBlob(info.get(), &fd));

  // Intentionally don't close the file descriptor: Unmount anyway.
  ASSERT_NO_FAILURES(test->Remount());
  // Just closing our local handle; the connection should be disconnected.
  int close_return = close(fd.release());
  int close_error = errno;
  ASSERT_EQ(-1, close_return);
  ASSERT_EQ(EPIPE, close_error);

  fd.reset(open(info->path, O_RDONLY));
  ASSERT_TRUE(fd, "Failed to open blob");
  ASSERT_TRUE(fs_test_utils::VerifyContents(fd.get(), info->data.get(), info->size_data));
  fd.reset();

  ASSERT_EQ(0, unlink(info->path));
}

TEST_F(BlobfsTest, UmountWithOpenFile) { RunUmountWithOpenFileTest(this); }

TEST_F(BlobfsTestWithFvm, UmountWithOpenFile) { RunUmountWithOpenFileTest(this); }

void RunUmountWithMappedFileTest(FilesystemTest* test) {
  std::unique_ptr<fs_test_utils::BlobInfo> info;
  ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(kMountPath, 1 << 16, &info));
  fbl::unique_fd fd;
  ASSERT_NO_FAILURES(MakeBlob(info.get(), &fd));

  void* addr = mmap(nullptr, info->size_data, PROT_READ, MAP_SHARED, fd.get(), 0);
  ASSERT_NOT_NULL(addr);
  fd.reset();

  // Intentionally don't unmap the file descriptor: Unmount anyway.
  ASSERT_NO_FAILURES(test->Remount());
  ASSERT_EQ(munmap(addr, info->size_data), 0);

  fd.reset(open(info->path, O_RDONLY));
  ASSERT_TRUE(fd.get(), "Failed to open blob");
  ASSERT_TRUE(fs_test_utils::VerifyContents(fd.get(), info->data.get(), info->size_data));
  ASSERT_EQ(0, unlink(info->path));
}

TEST_F(BlobfsTest, UmountWithMappedFile) { RunUmountWithMappedFileTest(this); }

TEST_F(BlobfsTestWithFvm, UmountWithMappedFile) { RunUmountWithMappedFileTest(this); }

void RunUmountWithOpenMappedFileTest(FilesystemTest* test) {
  std::unique_ptr<fs_test_utils::BlobInfo> info;
  ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(kMountPath, 1 << 16, &info));
  fbl::unique_fd fd;
  ASSERT_NO_FAILURES(MakeBlob(info.get(), &fd));

  void* addr = mmap(nullptr, info->size_data, PROT_READ, MAP_SHARED, fd.get(), 0);
  ASSERT_NOT_NULL(addr);

  // Intentionally don't close the file descriptor: Unmount anyway.
  ASSERT_NO_FAILURES(test->Remount());
  // Just closing our local handle; the connection should be disconnected.
  ASSERT_EQ(0, munmap(addr, info->size_data));
  int close_return = close(fd.release());
  int close_error = errno;
  ASSERT_EQ(-1, close_return);
  ASSERT_EQ(EPIPE, close_error);

  fd.reset(open(info->path, O_RDONLY));
  ASSERT_TRUE(fd.get(), "Failed to open blob");
  ASSERT_TRUE(fs_test_utils::VerifyContents(fd.get(), info->data.get(), info->size_data));
  ASSERT_EQ(0, unlink(info->path));
}

TEST_F(BlobfsTest, UmountWithOpenMappedFile) { RunUmountWithOpenMappedFileTest(this); }

TEST_F(BlobfsTestWithFvm, UmountWithOpenMappedFile) { RunUmountWithOpenMappedFileTest(this); }

void RunCreateUmountRemountSmallTest(FilesystemTest* test) {
  for (size_t i = 10; i < 16; i++) {
    std::unique_ptr<fs_test_utils::BlobInfo> info;
    ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(kMountPath, 1 << i, &info));

    fbl::unique_fd fd;
    ASSERT_NO_FAILURES(MakeBlob(info.get(), &fd));

    fd.reset();
    ASSERT_NO_FAILURES(test->Remount(), "Could not re-mount blobfs");

    fd.reset(open(info->path, O_RDONLY));
    ASSERT_TRUE(fd, "Failed to open blob");

    ASSERT_TRUE(fs_test_utils::VerifyContents(fd.get(), info->data.get(), info->size_data));
    ASSERT_EQ(0, unlink(info->path));
  }
}

TEST_F(BlobfsTest, CreateUmountRemountSmall) { RunCreateUmountRemountSmallTest(this); }

TEST_F(BlobfsTestWithFvm, CreateUmountRemountSmall) { RunCreateUmountRemountSmallTest(this); }

bool IsReadable(int fd) {
  char buf[1];
  return pread(fd, buf, sizeof(buf), 0) == sizeof(buf);
}

// Tests that we cannot read from the Blob until it has been fully written.
void RunEarlyReadTest() {
  std::unique_ptr<fs_test_utils::BlobInfo> info;

  ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(kMountPath, 1 << 17, &info));
  fbl::unique_fd fd(open(info->path, O_CREAT | O_EXCL | O_RDWR));
  ASSERT_TRUE(fd);

  // A second fd should also not be readable.
  fbl::unique_fd fd2(open(info->path, O_CREAT | O_RDWR));
  ASSERT_TRUE(fd2);

  ASSERT_FALSE(IsReadable(fd.get()), "Should not be readable after open");
  ASSERT_FALSE(IsReadable(fd2.get()), "Should not be readable after open");

  ASSERT_EQ(0, ftruncate(fd.get(), info->size_data));
  ASSERT_FALSE(IsReadable(fd.get()), "Should not be readable after alloc");
  ASSERT_FALSE(IsReadable(fd2.get()), "Should not be readable after alloc");

  ASSERT_EQ(0, fs_test_utils::StreamAll(write, fd.get(), info->data.get(), info->size_data),
            "Failed to write Data");

  // Okay, NOW we can read.
  // Double check that attempting to read early didn't cause problems...
  ASSERT_TRUE(fs_test_utils::VerifyContents(fd.get(), info->data.get(), info->size_data));
  ASSERT_TRUE(fs_test_utils::VerifyContents(fd2.get(), info->data.get(), info->size_data));

  ASSERT_TRUE(IsReadable(fd.get()));
}

TEST_F(BlobfsTest, EarlyRead) { RunEarlyReadTest(); }

TEST_F(BlobfsTestWithFvm, EarlyRead) { RunEarlyReadTest(); }

// Waits for up to 10 seconds until the file is readable. Returns 0 on success.
void CheckReadable(fbl::unique_fd fd, std::atomic<bool>* result) {
  struct pollfd fds;
  fds.fd = fd.get();
  fds.events = POLLIN;

  if (poll(&fds, 1, 10000) != 1) {
    printf("Failed to wait for readable blob\n");
    *result = false;
  }

  if (fds.revents != POLLIN) {
    printf("Unexpected event\n");
    *result = false;
  }

  if (!IsReadable(fd.get())) {
    printf("Not readable\n");
    *result = false;
  }

  *result = true;
}

// Tests that poll() can tell, at some point, when it's ok to read.
void RunWaitForReadTest() {
  std::unique_ptr<fs_test_utils::BlobInfo> info;

  ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(kMountPath, 1 << 17, &info));
  fbl::unique_fd fd(open(info->path, O_CREAT | O_EXCL | O_RDWR));
  ASSERT_TRUE(fd);

  {
    // Launch a background thread to wait for the file to become readable.
    std::atomic<bool> result;
    std::thread waiter_thread(CheckReadable, std::move(fd), &result);

    MakeBlob(info.get(), &fd);

    waiter_thread.join();
    ASSERT_TRUE(result.load(), "Background operation failed");
  }

  // Before continuing, make sure that MakeBlob was successful.
  ASSERT_NO_FAILURES();

  // Double check that attempting to read early didn't cause problems...
  ASSERT_TRUE(fs_test_utils::VerifyContents(fd.get(), info->data.get(), info->size_data));
}

TEST_F(BlobfsTest, WaitForRead) { RunWaitForReadTest(); }

TEST_F(BlobfsTestWithFvm, WaitForRead) { RunWaitForReadTest(); }

// Tests that seeks during writing are ignored.
void RunWriteSeekIgnoredTest() {
  srand(zxtest::Runner::GetInstance()->random_seed());
  std::unique_ptr<fs_test_utils::BlobInfo> info;
  ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(kMountPath, 1 << 17, &info));
  fbl::unique_fd fd(open(info->path, O_CREAT | O_RDWR));
  ASSERT_TRUE(fd, "Failed to create blob");
  ASSERT_EQ(0, ftruncate(fd.get(), info->size_data));

  off_t seek_pos = (rand() % info->size_data);
  ASSERT_EQ(seek_pos, lseek(fd.get(), seek_pos, SEEK_SET));
  ASSERT_EQ(info->size_data, write(fd.get(), info->data.get(), info->size_data));

  // Double check that attempting to seek early didn't cause problems...
  ASSERT_TRUE(fs_test_utils::VerifyContents(fd.get(), info->data.get(), info->size_data));
}

TEST_F(BlobfsTest, WriteSeekIgnored) { RunWriteSeekIgnoredTest(); }

TEST_F(BlobfsTestWithFvm, WriteSeekIgnored) { RunWriteSeekIgnoredTest(); }

void UnlinkAndRecreate(const char* path, fbl::unique_fd* fd) {
  ASSERT_EQ(0, unlink(path));
  fd->reset();  // Make sure the file is gone.
  fd->reset(open(path, O_CREAT | O_RDWR | O_EXCL));
  ASSERT_TRUE(*fd, "Failed to recreate blob");
}

// Try unlinking while creating a blob.
void RunRestartCreationTest() {
  std::unique_ptr<fs_test_utils::BlobInfo> info;
  ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(kMountPath, 1 << 17, &info));

  fbl::unique_fd fd(open(info->path, O_CREAT | O_RDWR));
  ASSERT_TRUE(fd, "Failed to create blob");

  // Unlink after first open.
  ASSERT_NO_FAILURES(UnlinkAndRecreate(info->path, &fd));

  // Unlink after init.
  ASSERT_EQ(0, ftruncate(fd.get(), info->size_data));
  ASSERT_NO_FAILURES(UnlinkAndRecreate(info->path, &fd));

  // Unlink after first write.
  ASSERT_EQ(0, ftruncate(fd.get(), info->size_data));
  ASSERT_EQ(0, fs_test_utils::StreamAll(write, fd.get(), info->data.get(), info->size_data),
            "Failed to write Data");
  ASSERT_NO_FAILURES(UnlinkAndRecreate(info->path, &fd));
}

TEST_F(BlobfsTest, RestartCreation) { RunRestartCreationTest(); }

TEST_F(BlobfsTestWithFvm, RestartCreation) { RunRestartCreationTest(); }

// Attempt using invalid operations.
void RunInvalidOperationsTest() {
  // First off, make a valid blob.
  std::unique_ptr<fs_test_utils::BlobInfo> info;
  ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(kMountPath, 1 << 12, &info));
  fbl::unique_fd fd;
  ASSERT_NO_FAILURES(MakeBlob(info.get(), &fd));
  ASSERT_TRUE(fs_test_utils::VerifyContents(fd.get(), info->data.get(), info->size_data));

  // Try some unsupported operations.
  ASSERT_LT(rename(info->path, info->path), 0);
  ASSERT_LT(truncate(info->path, 0), 0);
  ASSERT_LT(utime(info->path, nullptr), 0);

  // Test that a file cannot unmount the entire blobfs.
  zx_status_t status;
  fzl::FdioCaller caller(std::move(fd));
  ASSERT_OK(fuchsia_io_DirectoryAdminUnmount(caller.borrow_channel(), &status));
  ASSERT_EQ(ZX_ERR_ACCESS_DENIED, status);
  fd = caller.release();

  // Access the file once more, after these operations.
  ASSERT_TRUE(fs_test_utils::VerifyContents(fd.get(), info->data.get(), info->size_data));
}

TEST_F(BlobfsTest, InvalidOperations) { RunInvalidOperationsTest(); }

TEST_F(BlobfsTestWithFvm, InvalidOperations) { RunInvalidOperationsTest(); }

// Attempt operations on the root directory.
void RunRootDirectoryTest() {
  std::string name(kMountPath);
  name.append("/.");
  fbl::unique_fd dirfd(open(name.c_str(), O_RDONLY));
  ASSERT_TRUE(dirfd, "Cannot open root directory");

  std::unique_ptr<fs_test_utils::BlobInfo> info;
  ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(kMountPath, 1 << 12, &info));

  // Test operations which should ONLY operate on Blobs.
  ASSERT_LT(ftruncate(dirfd.get(), info->size_data), 0);

  char buf[8];
  ASSERT_LT(write(dirfd.get(), buf, 8), 0, "Should not write to directory");
  ASSERT_LT(read(dirfd.get(), buf, 8), 0, "Should not read from directory");

  // Should NOT be able to unlink root dir.
  ASSERT_LT(unlink(info->path), 0);
}

TEST_F(BlobfsTest, RootDirectory) { RunRootDirectoryTest(); }

TEST_F(BlobfsTestWithFvm, RootDirectory) { RunRootDirectoryTest(); }

void RunPartialWriteTest() {
  std::unique_ptr<fs_test_utils::BlobInfo> info_complete;
  std::unique_ptr<fs_test_utils::BlobInfo> info_partial;
  size_t size = 1 << 20;
  ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(kMountPath, size, &info_complete));
  ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(kMountPath, size, &info_partial));

  // Partially write out first blob.
  fbl::unique_fd fd_partial(open(info_partial->path, O_CREAT | O_RDWR));
  ASSERT_TRUE(fd_partial, "Failed to create blob");
  ASSERT_EQ(0, ftruncate(fd_partial.get(), size));
  ASSERT_EQ(0,
            fs_test_utils::StreamAll(write, fd_partial.get(), info_partial->data.get(), size / 2),
            "Failed to write Data");

  // Completely write out second blob.
  fbl::unique_fd fd_complete;
  ASSERT_NO_FAILURES(MakeBlob(info_complete.get(), &fd_complete));
}

TEST_F(BlobfsTest, PartialWrite) { RunPartialWriteTest(); }

TEST_F(BlobfsTestWithFvm, PartialWrite) { RunPartialWriteTest(); }

void RunPartialWriteSleepyDiskTest(const RamDisk* disk) {
  if (!disk) {
    return;
  }

  std::unique_ptr<fs_test_utils::BlobInfo> info_complete;
  std::unique_ptr<fs_test_utils::BlobInfo> info_partial;
  size_t size = 1 << 20;
  ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(kMountPath, size, &info_complete));
  ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(kMountPath, size, &info_partial));

  // Partially write out first blob.
  fbl::unique_fd fd_partial(open(info_partial->path, O_CREAT | O_RDWR));
  ASSERT_TRUE(fd_partial, "Failed to create blob");
  ASSERT_EQ(0, ftruncate(fd_partial.get(), size));
  ASSERT_EQ(0,
            fs_test_utils::StreamAll(write, fd_partial.get(), info_partial->data.get(), size / 2),
            "Failed to write Data");

  // Completely write out second blob.
  fbl::unique_fd fd_complete;
  ASSERT_NO_FAILURES(MakeBlob(info_complete.get(), &fd_complete));

  ASSERT_EQ(0, syncfs(fd_complete.get()));
  ASSERT_OK(disk->SleepAfter(0));

  fd_complete.reset(open(info_complete->path, O_RDONLY));
  ASSERT_TRUE(fd_complete, "Failed to re-open blob");

  ASSERT_EQ(0, syncfs(fd_complete.get()));
  ASSERT_OK(disk->WakeUp());

  ASSERT_TRUE(fs_test_utils::VerifyContents(fd_complete.get(), info_complete->data.get(), size));

  fd_partial.reset();
  fd_partial.reset(open(info_partial->path, O_RDONLY));
  ASSERT_FALSE(fd_partial, "Should not be able to open invalid blob");
}

TEST_F(BlobfsTest, PartialWriteSleepyDisk) {
  RunPartialWriteSleepyDiskTest(environment_->ramdisk());
}

TEST_F(BlobfsTestWithFvm, PartialWriteSleepyDisk) {
  RunPartialWriteSleepyDiskTest(environment_->ramdisk());
}

void RunMultipleWritesTest() {
  std::unique_ptr<fs_test_utils::BlobInfo> info;
  ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(kMountPath, 1 << 16, &info));

  fbl::unique_fd fd(open(info->path, O_CREAT | O_RDWR));
  ASSERT_TRUE(fd);

  ASSERT_EQ(0, ftruncate(fd.get(), info->size_data));

  const int kNumWrites = 128;
  size_t write_size = info->size_data / kNumWrites;
  for (size_t written = 0; written < info->size_data; written += write_size) {
    ASSERT_EQ(0, fs_test_utils::StreamAll(write, fd.get(), info->data.get() + written, write_size),
              "iteration %lu", written / write_size);
  }

  fd.reset();
  fd.reset(open(info->path, O_RDONLY));
  ASSERT_TRUE(fd);
  ASSERT_TRUE(fs_test_utils::VerifyContents(fd.get(), info->data.get(), info->size_data));
}

TEST_F(BlobfsTest, MultipleWrites) { RunMultipleWritesTest(); }

TEST_F(BlobfsTestWithFvm, MultipleWrites) { RunMultipleWritesTest(); }

/*

static bool TestHugeBlobRandom(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;
    fbl::unique_ptr<fs_test_utils::BlobInfo> info;

    // This blob is extremely large, and will remain large
    // on disk. It is not easily compressible.
    ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(
            MOUNT_PATH, 2 * blobfs::WriteBufferSize() * kBlobfsBlockSize(), &info));

    fbl::unique_fd fd;
    ASSERT_TRUE(MakeBlob(info.get(), &fd));
    ASSERT_EQ(close(fd.release()), 0);

    // We can re-open and verify the Blob as read-only
    fd.reset(open(info->path, O_RDONLY));
    ASSERT_TRUE(fd, "Failed to-reopen blob");
    ASSERT_TRUE(fs_test_utils::VerifyContents(fd.get(), info->data.get(), info->size_data));
    ASSERT_EQ(close(fd.release()), 0);

    // We cannot re-open the blob as writable
    fd.reset(open(info->path, O_RDWR | O_CREAT));
    ASSERT_FALSE(fd, "Shouldn't be able to re-create blob that exists");
    fd.reset(open(info->path, O_RDWR));
    ASSERT_FALSE(fd, "Shouldn't be able to re-open blob as writable");
    fd.reset(open(info->path, O_WRONLY));
    ASSERT_FALSE(fd, "Shouldn't be able to re-open blob as writable");

    // Force decompression by remounting, re-accessing blob.
    ASSERT_TRUE(blobfsTest->Remount());
    fd.reset(open(info->path, O_RDONLY));
    ASSERT_TRUE(fd, "Failed to-reopen blob");
    ASSERT_TRUE(fs_test_utils::VerifyContents(fd.get(), info->data.get(), info->size_data));
    ASSERT_EQ(close(fd.release()), 0);

    ASSERT_EQ(unlink(info->path), 0);
    END_HELPER;
}

static bool TestHugeBlobCompressible(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;
    fbl::unique_ptr<fs_test_utils::BlobInfo> info;

    // This blob is extremely large, and will remain large
    // on disk, even though is very compressible.
    ASSERT_TRUE(fs_test_utils::GenerateBlob([](char* data, size_t length) {
        fs_test_utils::RandomFill(data, length / 2);
        data = reinterpret_cast<char*>(reinterpret_cast<uintptr_t>(data) + length / 2);
        memset(data, 'a', length / 2);
    }, MOUNT_PATH, 2 * blobfs::WriteBufferSize() * kBlobfsBlockSize, &info));

    fbl::unique_fd fd;
    ASSERT_TRUE(MakeBlob(info.get(), &fd));
    ASSERT_EQ(close(fd.release()), 0);

    // We can re-open and verify the Blob as read-only
    fd.reset(open(info->path, O_RDONLY));
    ASSERT_TRUE(fd, "Failed to-reopen blob");
    ASSERT_TRUE(fs_test_utils::VerifyContents(fd.get(), info->data.get(), info->size_data));
    ASSERT_EQ(close(fd.release()), 0);

    // We cannot re-open the blob as writable
    fd.reset(open(info->path, O_RDWR | O_CREAT));
    ASSERT_FALSE(fd, "Shouldn't be able to re-create blob that exists");
    fd.reset(open(info->path, O_RDWR));
    ASSERT_FALSE(fd, "Shouldn't be able to re-open blob as writable");
    fd.reset(open(info->path, O_WRONLY));
    ASSERT_FALSE(fd, "Shouldn't be able to re-open blob as writable");

    // Force decompression by remounting, re-accessing blob.
    ASSERT_TRUE(blobfsTest->Remount());
    fd.reset(open(info->path, O_RDONLY));
    ASSERT_TRUE(fd, "Failed to-reopen blob");
    ASSERT_TRUE(fs_test_utils::VerifyContents(fd.get(), info->data.get(), info->size_data));
    ASSERT_EQ(close(fd.release()), 0);

    ASSERT_EQ(unlink(info->path), 0);
    END_HELPER;
}

static bool CreateUmountRemountLarge(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;
    fs_test_utils::BlobList bl(MOUNT_PATH);
    // TODO(smklein): Here, and elsewhere in this file, remove this source
    // of randomness to make the unit test deterministic -- fuzzing should
    // be the tool responsible for introducing randomness into the system.
    unsigned int seed = static_cast<unsigned int>(zx_ticks_get());
    unittest_printf("unmount_remount test using seed: %u\n", seed);

    // Do some operations...
    size_t num_ops = 5000;
    for (size_t i = 0; i < num_ops; ++i) {
        switch (rand_r(&seed) % 6) {
        case 0:
            ASSERT_TRUE(bl.CreateBlob(&seed));
            break;
        case 1:
            ASSERT_TRUE(bl.ConfigBlob());
            break;
        case 2:
            ASSERT_TRUE(bl.WriteData());
            break;
        case 3:
            ASSERT_TRUE(bl.ReadData());
            break;
        case 4:
            ASSERT_TRUE(bl.ReopenBlob());
            break;
        case 5:
            ASSERT_TRUE(bl.UnlinkBlob());
            break;
        }
    }

    // Close all currently opened nodes (REGARDLESS of their state)
    bl.CloseAll();

    // Unmount, remount
    ASSERT_TRUE(blobfsTest->Remount(), "Could not re-mount blobfs");

    // Reopen all (readable) blobs
    bl.OpenAll();

    // Verify state of all blobs
    bl.VerifyAll();

    // Close everything again
    bl.CloseAll();

    END_HELPER;
}

int unmount_remount_thread(void* arg) {
    fs_test_utils::BlobList* bl = static_cast<fs_test_utils::BlobList*>(arg);
    unsigned int seed = static_cast<unsigned int>(zx_ticks_get());
    unittest_printf("unmount_remount thread using seed: %u\n", seed);

    // Do some operations...
    size_t num_ops = 1000;
    for (size_t i = 0; i < num_ops; ++i) {
        switch (rand_r(&seed) % 6) {
        case 0:
            ASSERT_TRUE(bl->CreateBlob(&seed));
            break;
        case 1:
            ASSERT_TRUE(bl->ConfigBlob());
            break;
        case 2:
            ASSERT_TRUE(bl->WriteData());
            break;
        case 3:
            ASSERT_TRUE(bl->ReadData());
            break;
        case 4:
            ASSERT_TRUE(bl->ReopenBlob());
            break;
        case 5:
            ASSERT_TRUE(bl->UnlinkBlob());
            break;
        }
    }

    return 0;
}

static bool CreateUmountRemountLargeMultithreaded(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;
    fs_test_utils::BlobList bl(MOUNT_PATH);

    size_t num_threads = 10;
    fbl::AllocChecker ac;
    fbl::Array<thrd_t> threads(new (&ac) thrd_t[num_threads](), num_threads);
    ASSERT_TRUE(ac.check());

    // Launch all threads
    for (size_t i = 0; i < num_threads; i++) {
        ASSERT_EQ(thrd_create(&threads[i], unmount_remount_thread, &bl),
                  thrd_success);
    }

    // Wait for all threads to complete.
    // Currently, threads will always return a successful status.
    for (size_t i = 0; i < num_threads; i++) {
        int res;
        ASSERT_EQ(thrd_join(threads[i], &res), thrd_success);
        ASSERT_EQ(res, 0);
    }

    // Close all currently opened nodes (REGARDLESS of their state)
    bl.CloseAll();

    // Unmount, remount
    ASSERT_TRUE(blobfsTest->Remount(), "Could not re-mount blobfs");

    // reopen all blobs
    bl.OpenAll();

    // verify all blob contents
    bl.VerifyAll();

    // close everything again
    bl.CloseAll();

    END_HELPER;
}

static bool NoSpace(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;
    fbl::unique_ptr<fs_test_utils::BlobInfo> last_info = nullptr;

    // Keep generating blobs until we run out of space
    size_t count = 0;
    while (true) {
        fbl::unique_ptr<fs_test_utils::BlobInfo> info;
        ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, 1 << 17, &info));

        fbl::unique_fd fd(open(info->path, O_CREAT | O_RDWR));
        ASSERT_TRUE(fd, "Failed to create blob");
        int r = ftruncate(fd.get(), info->size_data);
        if (r < 0) {
            ASSERT_EQ(errno, ENOSPC, "Blobfs expected to run out of space");
            // We ran out of space, as expected. Can we allocate if we
            // unlink a previously allocated blob of the desired size?
            ASSERT_EQ(unlink(last_info->path), 0, "Unlinking old blob");
            ASSERT_EQ(ftruncate(fd.get(), info->size_data), 0, "Re-init after unlink");

            // Yay! allocated successfully.
            ASSERT_EQ(close(fd.release()), 0);
            break;
        }
        ASSERT_EQ(fs_test_utils::StreamAll(write, fd.get(), info->data.get(), info->size_data), 0,
                  "Failed to write Data");
        ASSERT_EQ(close(fd.release()), 0);
        last_info = std::move(info);

        if (++count % 50 == 0) {
            printf("Allocated %lu blobs\n", count);
        }
    }

    END_HELPER;
}

// The following test attempts to fragment the underlying blobfs partition
// assuming a trivial linear allocator. A more intelligent allocator
// may require modifications to this test.
static bool TestFragmentation(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;
    // Keep generating blobs until we run out of space, in a pattern of
    // large, small, large, small, large.
    //
    // At the end of  the test, we'll free the small blobs, and observe
    // if it is possible to allocate a larger blob. With a simple allocator
    // and no defragmentation, this would result in a NO_SPACE error.
    constexpr size_t kSmallSize = (1 << 16);
    constexpr size_t kLargeSize = (1 << 17);

    fbl::Vector<fbl::String> small_blobs;

    bool do_small_blob = true;
    size_t count = 0;
    while (true) {
        fbl::unique_ptr<fs_test_utils::BlobInfo> info;
        ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH,
                                                      do_small_blob ? kSmallSize : kLargeSize,
                                                      &info));
        fbl::unique_fd fd(open(info->path, O_CREAT | O_RDWR));
        ASSERT_TRUE(fd, "Failed to create blob");
        int r = ftruncate(fd.get(), info->size_data);
        if (r < 0) {
            ASSERT_EQ(ENOSPC, errno, "Blobfs expected to run out of space");
            break;
        }
        ASSERT_EQ(0, fs_test_utils::StreamAll(write, fd.get(), info->data.get(), info->size_data),
                  "Failed to write Data");
        ASSERT_EQ(0, close(fd.release()));
        if (do_small_blob) {
            small_blobs.push_back(fbl::String(info->path));
        }

        do_small_blob = !do_small_blob;

        if (++count % 50 == 0) {
            printf("Allocated %lu blobs\n", count);
        }
    }

    // We have filled up the disk with both small and large blobs.
    // Observe that we cannot add another large blob.
    fbl::unique_ptr<fs_test_utils::BlobInfo> info;
    ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, kLargeSize, &info));

    // Calculate actual number of blocks required to store the blob (including the merkle tree).
    blobfs::Inode large_inode;
    large_inode.blob_size = kLargeSize;
    size_t kLargeBlocks = blobfs::MerkleTreeBlocks(large_inode)
                          + blobfs::BlobDataBlocks(large_inode);

    // We shouldn't have space (before we try allocating) ...
    BlobfsUsage usage;
    ASSERT_TRUE(blobfsTest->CheckInfo(&usage));
    ASSERT_LT(usage.total_bytes - usage.used_bytes, kLargeBlocks * blobfs::kBlobfsBlockSize);

    // ... and we don't have space (as we try allocating).
    fbl::unique_fd fd(open(info->path, O_CREAT | O_RDWR));
    ASSERT_TRUE(fd);
    ASSERT_EQ(-1, ftruncate(fd.get(), info->size_data));
    ASSERT_EQ(ENOSPC, errno, "Blobfs expected to be out of space");

    // Unlink all small blobs -- except for the last one, since
    // we may have free trailing space at the end.
    for (size_t i = 0; i < small_blobs.size() - 1; i++) {
        ASSERT_EQ(0, unlink(small_blobs[i].c_str()), "Unlinking old blob");
    }

    // This asserts an assumption of our test: Freeing these blobs
    // should provde enough space.
    ASSERT_GT(kSmallSize * (small_blobs.size() - 1), kLargeSize);

    // Validate that we have enough space (before we try allocating)...
    ASSERT_TRUE(blobfsTest->CheckInfo(&usage));
    ASSERT_GE(usage.total_bytes - usage.used_bytes, kLargeBlocks * blobfs::kBlobfsBlockSize);

    // Now that blobfs supports extents, verify that we can still allocate
    // a large blob, even if it is fragmented.
    ASSERT_EQ(0, ftruncate(fd.get(), info->size_data));

    // Sanity check that we can write and read the fragmented blob.
    ASSERT_EQ(0, fs_test_utils::StreamAll(write, fd.get(), info->data.get(), info->size_data));
    fbl::unique_ptr<char[]> buf(new char[info->size_data]);
    ASSERT_EQ(0, lseek(fd.get(), 0, SEEK_SET));
    ASSERT_EQ(0, fs_test_utils::StreamAll(read, fd.get(), buf.get(), info->size_data));
    ASSERT_EQ(0, memcmp(info->data.get(), buf.get(), info->size_data));
    ASSERT_EQ(0, close(fd.release()));

    // Sanity check that we can re-open and unlink the fragmented blob.
    fd.reset(open(info->path, O_RDONLY));
    ASSERT_TRUE(fd);
    ASSERT_EQ(0, unlink(info->path));
    ASSERT_EQ(0, close(fd.release()));

    END_HELPER;
}

*/

zx_status_t DirectoryAdminGetDevicePath(fbl::unique_fd directory, std::string* path) {
  char buffer[fuchsia_io_MAX_PATH];
  zx_status_t status;
  size_t path_len;
  fzl::FdioCaller caller(std::move(directory));

  zx_status_t io_status = fuchsia_io_DirectoryAdminGetDevicePath(caller.borrow_channel(), &status,
                                                                 buffer, sizeof(buffer), &path_len);

  if (io_status != ZX_OK) {
    return io_status;
  }

  if (status != ZX_OK) {
    return status;
  }

  std::string device_path(buffer, path_len);
  path->assign(device_path);
  return ZX_OK;
}

void RunQueryDevicePathTest(const std::string& device_path) {
  fbl::unique_fd root_fd(open(kMountPath, O_RDONLY | O_ADMIN));
  ASSERT_TRUE(root_fd, "Cannot open root directory");

  std::string path;
  ASSERT_OK(DirectoryAdminGetDevicePath(std::move(root_fd), &path));
  ASSERT_FALSE(path.empty());

  ASSERT_STR_EQ(device_path.c_str(), path.c_str());
  printf("device_path %s\n", device_path.c_str());

  root_fd.reset(open(kMountPath, O_RDONLY));
  ASSERT_TRUE(root_fd, "Cannot open root directory");

  ASSERT_STATUS(ZX_ERR_ACCESS_DENIED, DirectoryAdminGetDevicePath(std::move(root_fd), &path));
}

TEST_F(BlobfsTest, QueryDevicePath) { RunQueryDevicePathTest(device_path()); }

TEST_F(BlobfsTestWithFvm, QueryDevicePath) {
  // Make sure the two paths to compare are in the same form.
  RunQueryDevicePathTest(GetTopologicalPath(device_path()));
}

void RunReadOnlyTest(FilesystemTest* test) {
  // Mount the filesystem as read-write. We can create new blobs.
  std::unique_ptr<fs_test_utils::BlobInfo> info;
  ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(kMountPath, 1 << 10, &info));
  fbl::unique_fd blob_fd;
  ASSERT_NO_FAILURES(MakeBlob(info.get(), &blob_fd));
  ASSERT_TRUE(fs_test_utils::VerifyContents(blob_fd.get(), info->data.get(), info->size_data));
  blob_fd.reset();

  test->set_read_only(true);
  ASSERT_NO_FAILURES(test->Remount());

  // We can read old blobs
  blob_fd.reset(open(info->path, O_RDONLY));
  ASSERT_TRUE(blob_fd);
  ASSERT_TRUE(fs_test_utils::VerifyContents(blob_fd.get(), info->data.get(), info->size_data));

  // We cannot create new blobs
  ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(kMountPath, 1 << 10, &info));
  blob_fd.reset(open(info->path, O_CREAT | O_RDWR));
  ASSERT_FALSE(blob_fd);
}

TEST_F(BlobfsTest, ReadOnly) { RunReadOnlyTest(this); }

TEST_F(BlobfsTestWithFvm, ReadOnly) { RunReadOnlyTest(this); }

// This tests growing both additional inodes and blocks
TEST_F(BlobfsTestWithFvm, ResizePartition) {
  // Create 1000 blobs. Test slices are small enough that this will require both inodes and
  // blocks to be added.
  // TODO(rvargas): Verify the number of used slices.
  for (int i = 0; i < 1000; i++) {
    std::unique_ptr<fs_test_utils::BlobInfo> info;
    ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(kMountPath, 64, &info));

    fbl::unique_fd fd;
    ASSERT_NO_FAILURES(MakeBlob(info.get(), &fd));
  }

  // Remount partition.
  ASSERT_NO_FAILURES(Remount(), "Could not re-mount blobfs");
}

/*

static bool CorruptAtMount(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;
    ASSERT_EQ(blobfsTest->GetType(), FsTestType::kFvm);
    ASSERT_TRUE(blobfsTest->Teardown());
    ASSERT_TRUE(blobfsTest->Reset());
    ASSERT_TRUE(blobfsTest->Init(FsTestState::kMinimal), "Mounting Blobfs");

    char device_path[PATH_MAX];
    ASSERT_TRUE(blobfsTest->GetDevicePath(device_path, PATH_MAX));
    ASSERT_EQ(mkfs(device_path, DISK_FORMAT_BLOBFS, launch_stdio_sync, &default_mkfs_options),
              ZX_OK);

    fbl::unique_fd fd(blobfsTest->GetFd());
    ASSERT_TRUE(fd, "Could not open ramdisk");
    fzl::UnownedFdioCaller caller(fd.get());

    // Manually shrink slice so FVM will differ from Blobfs.
    uint64_t offset = blobfs::kFVMNodeMapStart / kBlocksPerSlice;
    uint64_t length = 1;
    zx_status_t status;
    ASSERT_EQ(fuchsia_hardware_block_volume_VolumeShrink(caller.borrow_channel(), offset,
                                                         length, &status), ZX_OK);
    ASSERT_OK(status);

    // Verify that shrink was successful.
    uint64_t start_slices[1];
    start_slices[0] = offset;
    fuchsia_hardware_block_volume_VsliceRange
            ranges[fuchsia_hardware_block_volume_MAX_SLICE_REQUESTS];
    size_t actual_ranges_count;

    ASSERT_EQ(fuchsia_hardware_block_volume_VolumeQuerySlices(
                caller.borrow_channel(), start_slices, fbl::count_of(start_slices), &status,
                ranges, &actual_ranges_count), ZX_OK);
    ASSERT_OK(status);
    ASSERT_EQ(actual_ranges_count, 1);
    ASSERT_FALSE(ranges[0].allocated);
    ASSERT_EQ(ranges[0].count,
              (blobfs::kFVMJournalStart - blobfs::kFVMNodeMapStart) / kBlocksPerSlice);

    // Attempt to mount the VPart. This should fail since slices are missing.
    mount_options_t options = default_mount_options;
    options.enable_journal = gEnableJournal;
    caller.reset();
    ASSERT_NE(mount(fd.release(), MOUNT_PATH, DISK_FORMAT_BLOBFS, &options,
                    launch_stdio_async), ZX_OK);

    fd.reset(blobfsTest->GetFd());
    ASSERT_TRUE(fd, "Could not open ramdisk");
    caller.reset(fd.get());

    // Manually grow slice count to twice what it was initially.
    length = 2;
    ASSERT_EQ(fuchsia_hardware_block_volume_VolumeExtend(caller.borrow_channel(), offset,
                                                         length, &status), ZX_OK);
    ASSERT_OK(status);

    // Verify that extend was successful.
    ASSERT_EQ(fuchsia_hardware_block_volume_VolumeQuerySlices(
                caller.borrow_channel(), start_slices, fbl::count_of(start_slices), &status,
                ranges, &actual_ranges_count), ZX_OK);
    ASSERT_OK(status);
    ASSERT_EQ(actual_ranges_count, 1);
    ASSERT_TRUE(ranges[0].allocated);
    ASSERT_EQ(ranges[0].count, 2);

    // Attempt to mount the VPart. This should succeed.
    caller.reset();
    ASSERT_EQ(mount(fd.release(), MOUNT_PATH, DISK_FORMAT_BLOBFS, &options,
                    launch_stdio_async), ZX_OK);

    ASSERT_OK(umount(MOUNT_PATH));
    fd.reset(blobfsTest->GetFd());
    ASSERT_TRUE(fd, "Could not open ramdisk");
    caller.reset(fd.get());

    // Verify that mount automatically removed extra slice.
    ASSERT_EQ(fuchsia_hardware_block_volume_VolumeQuerySlices(
                caller.borrow_channel(), start_slices, fbl::count_of(start_slices), &status,
                ranges, &actual_ranges_count), ZX_OK);
    ASSERT_OK(status);
    ASSERT_EQ(actual_ranges_count, 1);
    ASSERT_TRUE(ranges[0].allocated);
    ASSERT_EQ(ranges[0].count, 1);
    END_HELPER;
}

typedef struct reopen_data {
    char path[PATH_MAX];
    std::atomic_bool complete;
} reopen_data_t;

int reopen_thread(void* arg) {
    reopen_data_t* dat = static_cast<reopen_data_t*>(arg);
    unsigned attempts = 0;
    while (!atomic_load(&dat->complete)) {
        fbl::unique_fd fd(open(dat->path, O_RDONLY));
        ASSERT_TRUE(fd);
        ASSERT_EQ(close(fd.release()), 0);
        attempts++;
    }

    printf("Reopened %u times\n", attempts);
    return 0;
}

// The purpose of this test is to repro the case where a blob is being retrieved from the blob hash
// at the same time it is being destructed, causing an invalid vnode to be returned. This can only
// occur when the client is opening a new fd to the blob at the same time it is being destructed
// after all writes to disk have completed.
// This test works best if a sleep is added at the beginning of fbl_recycle in VnodeBlob.
static bool CreateWriteReopen(BlobfsTest* blobfsTest) {
    BEGIN_HELPER;
    size_t num_ops = 10;

    fbl::unique_ptr<fs_test_utils::BlobInfo> anchor_info;
    ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, 1 << 10, &anchor_info));

    fbl::unique_ptr<fs_test_utils::BlobInfo> info;
    ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, 10 * (1 << 20), &info));
    reopen_data_t dat;
    strcpy(dat.path, info->path);

    for (size_t i = 0; i < num_ops; i++) {
        printf("Running op %lu... ", i);
        fbl::unique_fd fd;
        fbl::unique_fd anchor_fd;
        atomic_store(&dat.complete, false);

        // Write both blobs to disk (without verification, so we can start reopening the blob asap)
        ASSERT_TRUE(MakeBlobUnverified(info.get(), &fd));
        ASSERT_TRUE(MakeBlobUnverified(anchor_info.get(), &anchor_fd));
        ASSERT_EQ(close(fd.release()), 0);

        int result;
        int success;
        thrd_t thread;
        ASSERT_EQ(thrd_create(&thread, reopen_thread, &dat), thrd_success);

        {
            // In case the test fails, always join the thread before returning from the test.
            auto join_thread = fbl::MakeAutoCall([&]() {
                atomic_store(&dat.complete, true);
                success = thrd_join(thread, &result);
            });

            // Sleep while the thread continually opens and closes the blob
            usleep(1000000);
            ASSERT_EQ(syncfs(anchor_fd.get()), 0);
        }

        ASSERT_EQ(success, thrd_success);
        ASSERT_EQ(result, 0);

        ASSERT_EQ(close(anchor_fd.release()), 0);
        ASSERT_EQ(unlink(info->path), 0);
        ASSERT_EQ(unlink(anchor_info->path), 0);
    }

    END_HELPER;
}

static bool TestCreateFailure(void) {
    BEGIN_TEST;
    BlobfsTest blobfsTest(FsTestType::kNormal);
    blobfsTest.SetStdio(false);
    ASSERT_TRUE(blobfsTest.Init(), "Mounting Blobfs");

    fbl::unique_ptr<fs_test_utils::BlobInfo> info;
    ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, blobfs::kBlobfsBlockSize, &info));

    size_t blocks = 0;

    // Attempt to create a blob, failing after each written block until the operations succeeds.
    // After each failure, check for disk consistency.
    while (true) {
        ASSERT_TRUE(blobfsTest.ToggleSleep(blocks));
        unittest_set_output_function(silent_printf, nullptr);
        fbl::unique_fd fd;

        // Blob creation may or may not succeed - as long as fsck passes, it doesn't matter.
        MakeBlob(info.get(), &fd);
        current_test_info->all_ok = true;

        // Resolve all transactions before waking the ramdisk.
        syncfs(fd.get());
        unittest_restore_output_function();
        ASSERT_TRUE(blobfsTest.ToggleSleep());

        // Force remount so journal will replay.
        ASSERT_TRUE(blobfsTest.ForceRemount());

        // Remount again to check fsck results.
        ASSERT_TRUE(blobfsTest.Remount());

        // Once file creation is successful, break out of the loop.
        fd.reset(open(info->path, O_RDONLY));
        if (fd) break;

        blocks++;
    }

    ASSERT_TRUE(blobfsTest.Teardown(), "Unmounting Blobfs");
    END_TEST;
}

static bool TestExtendFailure(void) {
    BEGIN_TEST;
    BlobfsTest blobfsTest(FsTestType::kFvm);
    blobfsTest.SetStdio(false);
    ASSERT_TRUE(blobfsTest.Init(), "Mounting Blobfs");

    BlobfsUsage original_usage;
    blobfsTest.CheckInfo(&original_usage);

    // Create a blob of the maximum size possible without causing an FVM extension.
    fbl::unique_ptr<fs_test_utils::BlobInfo> old_info;
    ASSERT_TRUE(
        fs_test_utils::GenerateRandomBlob(MOUNT_PATH,
                                          original_usage.total_bytes - blobfs::kBlobfsBlockSize,
                                          &old_info));

    fbl::unique_fd fd;
    ASSERT_TRUE(MakeBlob(old_info.get(), &fd));
    ASSERT_EQ(syncfs(fd.get()), 0);
    ASSERT_EQ(close(fd.release()), 0);

    // Ensure that an FVM extension did not occur.
    BlobfsUsage current_usage;
    blobfsTest.CheckInfo(&current_usage);
    ASSERT_EQ(current_usage.total_bytes, original_usage.total_bytes);

    // Generate another blob of the smallest size possible.
    fbl::unique_ptr<fs_test_utils::BlobInfo> new_info;
    ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(MOUNT_PATH, blobfs::kBlobfsBlockSize,
                                                  &new_info));

    // Since the FVM metadata covers a large range of blocks, it will take a while to test a
    // ramdisk failure after each individual block. Since we mostly care about what happens with
    // blobfs after the extension succeeds on the FVM side, test a maximum of |metadata_failures|
    // failures within the FVM metadata write itself.
    size_t metadata_size = fvm::MetadataSize(blobfsTest.GetDiskSize(), kTestFvmSliceSize);
    size_t metadata_blocks = metadata_size / blobfsTest.GetBlockSize();
    size_t metadata_failures = 16;
    size_t increment = metadata_blocks / fbl::min(metadata_failures, metadata_blocks);

    // Round down the metadata block count so we don't miss testing the transaction immediately
    // after the metadata write succeeds.
    metadata_blocks = fbl::round_down(metadata_blocks, increment);
    size_t blocks = 0;

    while (true) {
        ASSERT_TRUE(blobfsTest.ToggleSleep(blocks));
        unittest_set_output_function(silent_printf, nullptr);

        // Blob creation may or may not succeed - as long as fsck passes, it doesn't matter.
        MakeBlob(new_info.get(), &fd);
        current_test_info->all_ok = true;

        // Resolve all transactions before waking the ramdisk.
        syncfs(fd.get());

        unittest_restore_output_function();
        ASSERT_TRUE(blobfsTest.ToggleSleep());

        // Force remount so journal will replay.
        ASSERT_TRUE(blobfsTest.ForceRemount());

        // Remount again to check fsck results.
        ASSERT_TRUE(blobfsTest.Remount());

        // Check that the original blob still exists.
        fd.reset(open(old_info->path, O_RDONLY));
        ASSERT_TRUE(fd);

        // Once file creation is successful, break out of the loop.
        fd.reset(open(new_info->path, O_RDONLY));
        if (fd) {
            struct stat stats;
            ASSERT_EQ(fstat(fd.get(), &stats), 0);
            ASSERT_EQ(static_cast<uint64_t>(stats.st_size), old_info->size_data);
            break;
        }

        if (blocks >= metadata_blocks) {
            blocks++;
        } else {
            blocks += increment;
        }
    }

    // Ensure that an FVM extension occurred.
    blobfsTest.CheckInfo(&current_usage);
    ASSERT_GT(current_usage.total_bytes, original_usage.total_bytes);

    ASSERT_TRUE(blobfsTest.Teardown(), "Unmounting Blobfs");
    END_TEST;
}

*/

void RunFailedWriteTest(const RamDisk* disk) {
  if (!disk) {
    return;
  }

  uint32_t page_size = disk->page_size();
  const uint32_t pages_per_block = blobfs::kBlobfsBlockSize / page_size;

  std::unique_ptr<fs_test_utils::BlobInfo> info;
  ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(kMountPath, blobfs::kBlobfsBlockSize, &info));

  fbl::unique_fd fd(open(info->path, O_CREAT | O_RDWR));
  ASSERT_TRUE(fd, "Failed to create blob");

  // Truncate before sleeping the ramdisk. This is so potential FVM updates
  // do not interfere with the ramdisk block count.
  ASSERT_EQ(ftruncate(fd.get(), info->size_data), 0);

  // Journal:
  // - One Superblock block
  // - One Inode table block
  // - One Bitmap block
  //
  // Non-journal:
  // - One Inode table block
  // - One Data block
  constexpr int kBlockCountToWrite = 5;

  // Sleep after |kBlockCountToWrite - 1| blocks. This is 1 less than will be
  // needed to write out the entire blob. This ensures that writing the blob
  // will ultimately fail, but the write operation will return a successful
  // response.
  ASSERT_OK(disk->SleepAfter(pages_per_block * (kBlockCountToWrite - 1)));
  ASSERT_EQ(write(fd.get(), info->data.get(), info->size_data),
            static_cast<ssize_t>(info->size_data));

  // Since the write operation ultimately failed when going out to disk,
  // syncfs will return a failed response.
  ASSERT_LT(syncfs(fd.get()), 0);

  ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(kMountPath, blobfs::kBlobfsBlockSize, &info));
  fd.reset(open(info->path, O_CREAT | O_RDWR));
  ASSERT_TRUE(fd, "Failed to create blob");

  // On an FVM, truncate may either succeed or fail. If an FVM extend call is necessary,
  // it will fail since the ramdisk is asleep; otherwise, it will pass.
  ftruncate(fd.get(), info->size_data);

  // Since the ramdisk is asleep and our blobfs is aware of it due to the sync, write should fail.
  // TODO(smklein): Implement support for "failed write propagates to the client before
  // sync".
  // ASSERT_LT(write(fd.get(), info->data.get(), blobfs::kBlobfsBlockSize), 0);

  ASSERT_OK(disk->WakeUp());
}

TEST_F(BlobfsTest, FailedWrite) {
  ASSERT_NO_FAILURES(RunFailedWriteTest(environment_->ramdisk()));

  // Force journal replay.
  Remount();
}

TEST_F(BlobfsTestWithFvm, FailedWrite) {
  ASSERT_NO_FAILURES(RunFailedWriteTest(environment_->ramdisk()));

  // Force journal replay.
  Remount();
}

class LargeBlobTest : public BlobfsFixedDiskSizeTest {
 public:
  LargeBlobTest() : BlobfsFixedDiskSizeTest(GetDiskSize()) {}

  static uint64_t GetDataBlockCount() { return 12 * blobfs::kBlobfsBlockBits / 10; }

 private:
  static uint64_t GetDiskSize() {
    // Create blobfs with enough data blocks to ensure 2 block bitmap blocks.
    // Any number above kBlobfsBlockBits should do, and the larger the
    // number, the bigger the disk (and memory used for the test).
    blobfs::Superblock superblock;
    superblock.flags = 0;
    superblock.inode_count = blobfs::kBlobfsDefaultInodeCount;
    superblock.journal_block_count = blobfs::kDefaultJournalBlocks;
    superblock.data_block_count = GetDataBlockCount();
    return blobfs::TotalBlocks(superblock) * blobfs::kBlobfsBlockSize;
  }
};

TEST_F(LargeBlobTest, UseSecondBitmap) {
  // Create (and delete) a blob large enough to overflow into the second bitmap block.
  std::unique_ptr<fs_test_utils::BlobInfo> info;
  size_t blob_size = ((GetDataBlockCount() / 2) + 1) * blobfs::kBlobfsBlockSize;
  ASSERT_TRUE(fs_test_utils::GenerateRandomBlob(kMountPath, blob_size, &info));

  fbl::unique_fd fd;
  ASSERT_NO_FAILURES(MakeBlob(info.get(), &fd));
  ASSERT_EQ(syncfs(fd.get()), 0);
  ASSERT_EQ(close(fd.release()), 0);
  ASSERT_EQ(unlink(info->path), 0);
}

}  // namespace

/*

BEGIN_TEST_CASE(blobfs_tests)
RUN_TESTS_SILENT(MEDIUM, CorruptedBlob)
RUN_TESTS_SILENT(MEDIUM, CorruptedDigest)
RUN_TESTS(LARGE, TestHugeBlobRandom)
RUN_TESTS(LARGE, TestHugeBlobCompressible)
RUN_TESTS(LARGE, CreateUmountRemountLarge)
RUN_TESTS(LARGE, CreateUmountRemountLargeMultithreaded)
RUN_TESTS(LARGE, NoSpace)
RUN_TESTS(LARGE, TestFragmentation)
RUN_TEST_FVM(MEDIUM, CorruptAtMount)
RUN_TESTS(LARGE, CreateWriteReopen)
RUN_TEST_MEDIUM(TestCreateFailure)
RUN_TEST_MEDIUM(TestExtendFailure)
END_TEST_CASE(blobfs_tests)

*/
