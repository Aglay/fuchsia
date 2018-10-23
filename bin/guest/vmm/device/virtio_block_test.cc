// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fbl/unique_fd.h>
#include <lib/fzl/fdio.h>
#include <virtio/block.h>

#include "garnet/bin/guest/vmm/device/test_with_device.h"
#include "garnet/bin/guest/vmm/device/virtio_queue_fake.h"
#include "garnet/lib/machina/device/block.h"

static constexpr char kVirtioBlockUrl[] = "virtio_block";
static constexpr uint16_t kNumQueues = 1;
static constexpr uint16_t kQueueSize = 16;

static constexpr char kVirtioBlockId[] = "block-id";
static constexpr size_t kNumSectors = 2;
static constexpr uint8_t kSectorBytes[kNumSectors] = {0xab, 0xcd};

class VirtioBlockTest : public TestWithDevice {
 protected:
  VirtioBlockTest()
      : request_queue_(phys_mem_, PAGE_SIZE * kNumQueues, kQueueSize) {}

  void SetUp() override {
    // Launch device process.
    fuchsia::guest::device::StartInfo start_info;
    zx_status_t status =
        LaunchDevice(kVirtioBlockUrl, request_queue_.end(), &start_info);
    ASSERT_EQ(ZX_OK, status);

    // Setup block file.
    char path_template[] = "/tmp/block.XXXXXX";
    fbl::unique_fd fd = CreateBlockFile(path_template);
    ASSERT_TRUE(fd);
    fzl::FdioCaller fdio(std::move(fd));
    fuchsia::io::FilePtr file;
    file.Bind(zx::channel(fdio.borrow_channel()));

    // Start device execution.
    services.ConnectToService(block_.NewRequest());
    uint64_t size;
    status = block_->Start(std::move(start_info), kVirtioBlockId,
                           fuchsia::guest::device::BlockMode::READ_WRITE,
                           fuchsia::guest::device::BlockFormat::RAW,
                           std::move(file), &size);
    ASSERT_EQ(ZX_OK, status);
    ASSERT_EQ(machina::kBlockSectorSize * kNumSectors, size);

    // Configure device queues.
    VirtioQueueFake* queues[kNumQueues] = {&request_queue_};
    for (size_t i = 0; i < kNumQueues; i++) {
      auto q = queues[i];
      q->Configure(PAGE_SIZE * i, PAGE_SIZE);
      status = block_->ConfigureQueue(i, q->size(), q->desc(), q->avail(),
                                      q->used());
      ASSERT_EQ(ZX_OK, status);
    }
  }

  fuchsia::guest::device::VirtioBlockSyncPtr block_;
  VirtioQueueFake request_queue_;

 private:
  fbl::unique_fd CreateBlockFile(char* path) {
    fbl::unique_fd fd(mkstemp(path));
    if (!fd) {
      FXL_LOG(ERROR) << "Failed to create " << path << ": " << strerror(errno);
      return fd;
    }
    std::vector<uint8_t> buf(machina::kBlockSectorSize * kNumSectors);
    auto addr = buf.data();
    for (uint8_t byte : kSectorBytes) {
      memset(addr, byte, machina::kBlockSectorSize);
      addr += machina::kBlockSectorSize;
    }
    ssize_t ret = pwrite(fd.get(), buf.data(), buf.size(), 0);
    if (ret < 0) {
      FXL_LOG(ERROR) << "Failed to zero " << path << ": " << strerror(errno);
      fd.reset();
    }
    return fd;
  }
};

TEST_F(VirtioBlockTest, BadHeaderShort) {
  uint8_t header[sizeof(virtio_blk_req_t) - 1] = {};
  uint8_t* blk_status;
  zx_status_t status =
      DescriptorChainBuilder(request_queue_)
          .AppendReadableDescriptor(header, sizeof(header))
          .AppendWritableDescriptor(&blk_status, sizeof(*blk_status))
          .Build();
  ASSERT_EQ(ZX_OK, status);

  status = block_->NotifyQueue(0);
  ASSERT_EQ(ZX_OK, status);
  status = WaitOnInterrupt();
  ASSERT_EQ(ZX_OK, status);

  EXPECT_EQ(VIRTIO_BLK_S_IOERR, *blk_status);
}

TEST_F(VirtioBlockTest, BadHeaderLong) {
  uint8_t header[sizeof(virtio_blk_req_t) + 1] = {};
  uint8_t* blk_status;
  zx_status_t status =
      DescriptorChainBuilder(request_queue_)
          .AppendReadableDescriptor(header, sizeof(header))
          .AppendWritableDescriptor(&blk_status, sizeof(*blk_status))
          .Build();
  ASSERT_EQ(ZX_OK, status);

  status = block_->NotifyQueue(0);
  ASSERT_EQ(ZX_OK, status);
  status = WaitOnInterrupt();
  ASSERT_EQ(ZX_OK, status);

  EXPECT_EQ(VIRTIO_BLK_S_IOERR, *blk_status);
}

TEST_F(VirtioBlockTest, BadPayload) {
  virtio_blk_req_t header = {
      .type = VIRTIO_BLK_T_IN,
  };
  uint8_t* sector;
  uint8_t* blk_status;
  zx_status_t status =
      DescriptorChainBuilder(request_queue_)
          .AppendReadableDescriptor(&header, sizeof(header))
          .AppendWritableDescriptor(&sector, machina::kBlockSectorSize + 1)
          .AppendWritableDescriptor(&blk_status, sizeof(*blk_status))
          .Build();
  ASSERT_EQ(ZX_OK, status);

  status = block_->NotifyQueue(0);
  ASSERT_EQ(ZX_OK, status);
  status = WaitOnInterrupt();
  ASSERT_EQ(ZX_OK, status);

  EXPECT_EQ(VIRTIO_BLK_S_IOERR, *blk_status);
}

TEST_F(VirtioBlockTest, BadStatus) {
  virtio_blk_req_t header = {
      .type = VIRTIO_BLK_T_IN,
  };
  uint8_t* sector;
  uint8_t* blk_status;
  zx_status_t status =
      DescriptorChainBuilder(request_queue_)
          .AppendReadableDescriptor(&header, sizeof(header))
          .AppendWritableDescriptor(&sector, machina::kBlockSectorSize)
          .AppendWritableDescriptor(&blk_status, 2)
          .Build();
  ASSERT_EQ(ZX_OK, status);
  *blk_status = UINT8_MAX;

  status = block_->NotifyQueue(0);
  ASSERT_EQ(ZX_OK, status);
  status = WaitOnInterrupt();
  ASSERT_EQ(ZX_OK, status);

  EXPECT_EQ(UINT8_MAX, *blk_status);
}

TEST_F(VirtioBlockTest, BadRequestType) {
  virtio_blk_req_t header = {
      .type = UINT32_MAX,
  };
  uint8_t* blk_status;
  zx_status_t status =
      DescriptorChainBuilder(request_queue_)
          .AppendReadableDescriptor(&header, sizeof(header))
          .AppendWritableDescriptor(&blk_status, sizeof(*blk_status))
          .Build();
  ASSERT_EQ(ZX_OK, status);

  status = block_->NotifyQueue(0);
  ASSERT_EQ(ZX_OK, status);
  status = WaitOnInterrupt();
  ASSERT_EQ(ZX_OK, status);

  EXPECT_EQ(VIRTIO_BLK_S_UNSUPP, *blk_status);
}

TEST_F(VirtioBlockTest, Read) {
  virtio_blk_req_t header = {
      .type = VIRTIO_BLK_T_IN,
  };
  uint8_t* sector;
  uint8_t* blk_status;
  zx_status_t status =
      DescriptorChainBuilder(request_queue_)
          .AppendReadableDescriptor(&header, sizeof(header))
          .AppendWritableDescriptor(&sector, machina::kBlockSectorSize)
          .AppendWritableDescriptor(&blk_status, sizeof(*blk_status))
          .Build();
  ASSERT_EQ(ZX_OK, status);

  status = block_->NotifyQueue(0);
  ASSERT_EQ(ZX_OK, status);
  status = WaitOnInterrupt();
  ASSERT_EQ(ZX_OK, status);

  EXPECT_EQ(VIRTIO_BLK_S_OK, *blk_status);
  for (size_t i = 0; i < machina::kBlockSectorSize; i++) {
    EXPECT_EQ(kSectorBytes[0], sector[i]) << " mismatched byte " << i;
  }
}

TEST_F(VirtioBlockTest, ReadMultipleDescriptors) {
  virtio_blk_req_t header = {
      .type = VIRTIO_BLK_T_IN,
  };
  uint8_t* sector_1;
  uint8_t* sector_2;
  uint8_t* blk_status;
  zx_status_t status =
      DescriptorChainBuilder(request_queue_)
          .AppendReadableDescriptor(&header, sizeof(header))
          .AppendWritableDescriptor(&sector_1, machina::kBlockSectorSize)
          .AppendWritableDescriptor(&sector_2, machina::kBlockSectorSize)
          .AppendWritableDescriptor(&blk_status, sizeof(*blk_status))
          .Build();
  ASSERT_EQ(ZX_OK, status);

  status = block_->NotifyQueue(0);
  ASSERT_EQ(ZX_OK, status);
  status = WaitOnInterrupt();
  ASSERT_EQ(ZX_OK, status);

  EXPECT_EQ(VIRTIO_BLK_S_OK, *blk_status);
  for (size_t i = 0; i < machina::kBlockSectorSize; i++) {
    EXPECT_EQ(kSectorBytes[0], sector_1[i]) << " mismatched byte " << i;
    EXPECT_EQ(kSectorBytes[1], sector_2[i]) << " mismatched byte " << i;
  }
}

TEST_F(VirtioBlockTest, Write) {
  virtio_blk_req_t header = {
      .type = VIRTIO_BLK_T_OUT,
  };
  std::vector<uint8_t> sector(machina::kBlockSectorSize, UINT8_MAX);
  uint8_t* blk_status;
  zx_status_t status =
      DescriptorChainBuilder(request_queue_)
          .AppendReadableDescriptor(&header, sizeof(header))
          .AppendReadableDescriptor(sector.data(), sector.size())
          .AppendWritableDescriptor(&blk_status, sizeof(*blk_status))
          .Build();
  ASSERT_EQ(ZX_OK, status);

  status = block_->NotifyQueue(0);
  ASSERT_EQ(ZX_OK, status);
  status = WaitOnInterrupt();
  ASSERT_EQ(ZX_OK, status);

  EXPECT_EQ(VIRTIO_BLK_S_OK, *blk_status);
}

TEST_F(VirtioBlockTest, WriteMultipleDescriptors) {
  virtio_blk_req_t header = {
      .type = VIRTIO_BLK_T_OUT,
  };
  std::vector<uint8_t> sector_1(machina::kBlockSectorSize, UINT8_MAX);
  std::vector<uint8_t> sector_2(machina::kBlockSectorSize, UINT8_MAX);
  uint8_t* blk_status;
  zx_status_t status =
      DescriptorChainBuilder(request_queue_)
          .AppendReadableDescriptor(&header, sizeof(header))
          .AppendReadableDescriptor(sector_1.data(), sector_1.size())
          .AppendReadableDescriptor(sector_2.data(), sector_2.size())
          .AppendWritableDescriptor(&blk_status, sizeof(*blk_status))
          .Build();
  ASSERT_EQ(ZX_OK, status);

  status = block_->NotifyQueue(0);
  ASSERT_EQ(ZX_OK, status);
  status = WaitOnInterrupt();
  ASSERT_EQ(ZX_OK, status);

  EXPECT_EQ(VIRTIO_BLK_S_OK, *blk_status);
}

TEST_F(VirtioBlockTest, Sync) {
  virtio_blk_req_t header = {
      .type = VIRTIO_BLK_T_FLUSH,
  };
  uint8_t* blk_status;
  zx_status_t status =
      DescriptorChainBuilder(request_queue_)
          .AppendReadableDescriptor(&header, sizeof(header))
          .AppendWritableDescriptor(&blk_status, sizeof(*blk_status))
          .Build();
  ASSERT_EQ(ZX_OK, status);

  status = block_->NotifyQueue(0);
  ASSERT_EQ(ZX_OK, status);
  status = WaitOnInterrupt();
  ASSERT_EQ(ZX_OK, status);

  EXPECT_EQ(VIRTIO_BLK_S_OK, *blk_status);
}

TEST_F(VirtioBlockTest, SyncWithData) {
  virtio_blk_req_t header = {
      .type = VIRTIO_BLK_T_FLUSH,
  };
  std::vector<uint8_t> sector(machina::kBlockSectorSize);
  uint8_t* blk_status;
  zx_status_t status =
      DescriptorChainBuilder(request_queue_)
          .AppendReadableDescriptor(&header, sizeof(header))
          .AppendReadableDescriptor(sector.data(), sector.size())
          .AppendWritableDescriptor(&blk_status, sizeof(*blk_status))
          .Build();
  ASSERT_EQ(ZX_OK, status);

  status = block_->NotifyQueue(0);
  ASSERT_EQ(ZX_OK, status);
  status = WaitOnInterrupt();
  ASSERT_EQ(ZX_OK, status);

  EXPECT_EQ(VIRTIO_BLK_S_OK, *blk_status);
}

TEST_F(VirtioBlockTest, SyncNonZeroSector) {
  virtio_blk_req_t header = {
      .type = VIRTIO_BLK_T_FLUSH,
      .sector = 1,
  };
  uint8_t* blk_status;
  zx_status_t status =
      DescriptorChainBuilder(request_queue_)
          .AppendReadableDescriptor(&header, sizeof(header))
          .AppendWritableDescriptor(&blk_status, sizeof(*blk_status))
          .Build();
  ASSERT_EQ(ZX_OK, status);

  status = block_->NotifyQueue(0);
  ASSERT_EQ(ZX_OK, status);
  status = WaitOnInterrupt();
  ASSERT_EQ(ZX_OK, status);

  EXPECT_EQ(VIRTIO_BLK_S_IOERR, *blk_status);
}

TEST_F(VirtioBlockTest, Id) {
  virtio_blk_req_t header = {
      .type = VIRTIO_BLK_T_GET_ID,
  };
  char* id;
  uint8_t* blk_status;
  zx_status_t status =
      DescriptorChainBuilder(request_queue_)
          .AppendReadableDescriptor(&header, sizeof(header))
          .AppendWritableDescriptor(&id, VIRTIO_BLK_ID_BYTES)
          .AppendWritableDescriptor(&blk_status, sizeof(*blk_status))
          .Build();
  ASSERT_EQ(ZX_OK, status);

  status = block_->NotifyQueue(0);
  ASSERT_EQ(ZX_OK, status);
  status = WaitOnInterrupt();
  ASSERT_EQ(ZX_OK, status);

  EXPECT_EQ(VIRTIO_BLK_S_OK, *blk_status);
  EXPECT_EQ(0, memcmp(id, kVirtioBlockId, sizeof(kVirtioBlockId)));
}

TEST_F(VirtioBlockTest, IdLengthIncorrect) {
  virtio_blk_req_t header = {
      .type = VIRTIO_BLK_T_GET_ID,
  };
  char* id;
  uint8_t* blk_status;
  zx_status_t status =
      DescriptorChainBuilder(request_queue_)
          .AppendReadableDescriptor(&header, sizeof(header))
          .AppendWritableDescriptor(&id, VIRTIO_BLK_ID_BYTES + 1)
          .AppendWritableDescriptor(&blk_status, sizeof(*blk_status))
          .Build();
  ASSERT_EQ(ZX_OK, status);

  status = block_->NotifyQueue(0);
  ASSERT_EQ(ZX_OK, status);
  status = WaitOnInterrupt();
  ASSERT_EQ(ZX_OK, status);

  EXPECT_EQ(VIRTIO_BLK_S_IOERR, *blk_status);
}