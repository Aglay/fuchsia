// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "device-partitioner.h"

#include <dirent.h>
#include <fcntl.h>

#include <fbl/auto_call.h>
#include <fbl/unique_ptr.h>
#include <fuchsia/hardware/nand/c/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/devmgr-integration-test/fixture.h>
#include <zircon/boot/image.h>
#include <zircon/hw/gpt.h>
#include <zircon/syscalls.h>
#include <zircon/types.h>
#include <zxtest/zxtest.h>

#include <utility>

#include "test/test-utils.h"

namespace {

using devmgr_integration_test::IsolatedDevmgr;
using devmgr_integration_test::RecursiveWaitForFile;

constexpr uint8_t kZirconAType[GPT_GUID_LEN] = GUID_ZIRCON_A_VALUE;
constexpr uint8_t kZirconBType[GPT_GUID_LEN] = GUID_ZIRCON_B_VALUE;
constexpr uint8_t kZirconRType[GPT_GUID_LEN] = GUID_ZIRCON_R_VALUE;
constexpr uint8_t kVbMetaAType[GPT_GUID_LEN] = GUID_VBMETA_A_VALUE;
constexpr uint8_t kVbMetaBType[GPT_GUID_LEN] = GUID_VBMETA_B_VALUE;
constexpr uint8_t kFvmType[GPT_GUID_LEN] = GUID_FVM_VALUE;

constexpr fuchsia_hardware_nand_RamNandInfo
    kNandInfo =
        {
            .vmo = ZX_HANDLE_INVALID,
            .nand_info =
                {
                    .page_size = kPageSize,
                    .pages_per_block = kPagesPerBlock,
                    .num_blocks = kNumBlocks,
                    .ecc_bits = 8,
                    .oob_size = kOobSize,
                    .nand_class = fuchsia_hardware_nand_Class_PARTMAP,
                    .partition_guid = {},
                },
            .partition_map =
                {
                    .device_guid = {},
                    .partition_count = 7,
                    .partitions =
                        {
                            {
                                .type_guid = {},
                                .unique_guid = {},
                                .first_block = 0,
                                .last_block = 3,
                                .copy_count = 0,
                                .copy_byte_offset = 0,
                                .name = {},
                                .hidden = true,
                                .bbt = true,
                            },
                            {
                                .type_guid = GUID_BOOTLOADER_VALUE,
                                .unique_guid = {},
                                .first_block = 4,
                                .last_block = 7,
                                .copy_count = 0,
                                .copy_byte_offset = 0,
                                .name = {'b', 'o', 'o', 't', 'l', 'o', 'a', 'd', 'e', 'r'},
                                .hidden = false,
                                .bbt = false,
                            },
                            {
                                .type_guid = GUID_ZIRCON_A_VALUE,
                                .unique_guid = {},
                                .first_block = 8,
                                .last_block = 9,
                                .copy_count = 0,
                                .copy_byte_offset = 0,
                                .name = {'z', 'i', 'r', 'c', 'o', 'n', '-', 'a'},
                                .hidden = false,
                                .bbt = false,
                            },
                            {
                                .type_guid = GUID_ZIRCON_B_VALUE,
                                .unique_guid = {},
                                .first_block = 10,
                                .last_block = 11,
                                .copy_count = 0,
                                .copy_byte_offset = 0,
                                .name = {'z', 'i', 'r', 'c', 'o', 'n', '-', 'b'},
                                .hidden = false,
                                .bbt = false,
                            },
                            {
                                .type_guid = GUID_ZIRCON_R_VALUE,
                                .unique_guid = {},
                                .first_block = 12,
                                .last_block = 13,
                                .copy_count = 0,
                                .copy_byte_offset = 0,
                                .name = {'z', 'i', 'r', 'c', 'o', 'n', '-', 'r'},
                                .hidden = false,
                                .bbt = false,
                            },
                            {
                                .type_guid = GUID_VBMETA_A_VALUE,
                                .unique_guid = {},
                                .first_block = 14,
                                .last_block = 15,
                                .copy_count = 0,
                                .copy_byte_offset = 0,
                                .name = {'v', 'b', 'm', 'e', 't', 'a', '-', 'a'},
                                .hidden = false,
                                .bbt = false,
                            },
                            {
                                .type_guid = GUID_VBMETA_B_VALUE,
                                .unique_guid = {},
                                .first_block = 16,
                                .last_block = 17,
                                .copy_count = 0,
                                .copy_byte_offset = 0,
                                .name = {'v', 'b', 'm', 'e', 't', 'a', '-', 'b'},
                                .hidden = false,
                                .bbt = false,
                            },
                        },
                },
            .export_nand_config = true,
            .export_partition_map = true,
};

} // namespace

TEST(FixedDevicePartitionerTests, UseBlockInterfaceTest) {
    // TODO(ZX-4719): fix and re-enable this test.
    return;
    fbl::unique_fd devfs(open("/dev", O_RDWR));
    fbl::unique_ptr<paver::DevicePartitioner> partitioner;
    ASSERT_OK(paver::FixedDevicePartitioner::Initialize(std::move(devfs), &partitioner));
    ASSERT_FALSE(partitioner->UseSkipBlockInterface());
}

TEST(FixedDevicePartitionerTests, AddPartitionTest) {
    // TODO(ZX-4719): fix and re-enable this test.
    return;
    fbl::unique_fd devfs(open("/dev", O_RDWR));
    fbl::unique_ptr<paver::DevicePartitioner> partitioner;
    ASSERT_OK(paver::FixedDevicePartitioner::Initialize(std::move(devfs), &partitioner));
    ASSERT_EQ(partitioner->AddPartition(paver::Partition::kZirconB, nullptr), ZX_ERR_NOT_SUPPORTED);
}

TEST(FixedDevicePartitionerTests, WipeFvmTest) {
    // TODO(ZX-4719): fix and re-enable this test.
    return;
    fbl::unique_fd devfs(open("/dev", O_RDWR));
    fbl::unique_ptr<paver::DevicePartitioner> partitioner;
    ASSERT_OK(paver::FixedDevicePartitioner::Initialize(std::move(devfs), &partitioner));
    ASSERT_OK(partitioner->WipeFvm());
}

TEST(FixedDevicePartitionerTests, FinalizePartitionTest) {
    // TODO(ZX-4719): fix and re-enable this test.
    return;
    fbl::unique_fd devfs(open("/dev", O_RDWR));
    fbl::unique_ptr<paver::DevicePartitioner> partitioner;
    ASSERT_OK(paver::FixedDevicePartitioner::Initialize(std::move(devfs), &partitioner));

    ASSERT_OK(partitioner->FinalizePartition(paver::Partition::kZirconA));
    ASSERT_OK(partitioner->FinalizePartition(paver::Partition::kZirconB));
    ASSERT_OK(partitioner->FinalizePartition(paver::Partition::kZirconR));
    ASSERT_OK(partitioner->FinalizePartition(paver::Partition::kVbMetaA));
    ASSERT_OK(partitioner->FinalizePartition(paver::Partition::kVbMetaB));
    ASSERT_OK(partitioner->FinalizePartition(paver::Partition::kFuchsiaVolumeManager));
}

void CreateIsolatedDevmgr(IsolatedDevmgr* out) {
    devmgr_launcher::Args args;
    args.sys_device_driver = IsolatedDevmgr::kSysdevDriver;
    args.driver_search_paths.push_back("/boot/driver");
    args.use_system_svchost = true;
    args.disable_block_watcher = true;
    ASSERT_OK(IsolatedDevmgr::Create(std::move(args), out));

    fbl::unique_fd fd;
    ASSERT_OK(RecursiveWaitForFile(out->devfs_root(), "misc/ramctl", &fd));
}

TEST(FixedDevicePartitionerTests, FindPartitionTest) {
    IsolatedDevmgr devmgr;
    CreateIsolatedDevmgr(&devmgr);

    fbl::unique_ptr<BlockDevice> fvm, zircon_a, zircon_b, zircon_r, vbmeta_a, vbmeta_b;
    ASSERT_NO_FATAL_FAILURES(BlockDevice::Create(devmgr.devfs_root(), kZirconAType, &zircon_a));
    ASSERT_NO_FATAL_FAILURES(BlockDevice::Create(devmgr.devfs_root(), kZirconBType, &zircon_b));
    ASSERT_NO_FATAL_FAILURES(BlockDevice::Create(devmgr.devfs_root(), kZirconRType, &zircon_r));
    ASSERT_NO_FATAL_FAILURES(BlockDevice::Create(devmgr.devfs_root(), kVbMetaAType, &vbmeta_a));
    ASSERT_NO_FATAL_FAILURES(BlockDevice::Create(devmgr.devfs_root(), kVbMetaBType, &vbmeta_b));
    ASSERT_NO_FATAL_FAILURES(BlockDevice::Create(devmgr.devfs_root(), kFvmType, &fvm));

    auto partitioner = paver::DevicePartitioner::Create(devmgr.devfs_root().duplicate(),
                                                        paver::Arch::kArm64);
    ASSERT_NE(partitioner.get(), nullptr);

    fbl::unique_fd fd;
    ASSERT_OK(partitioner->FindPartition(paver::Partition::kZirconA, &fd));
    ASSERT_OK(partitioner->FindPartition(paver::Partition::kZirconB, &fd));
    ASSERT_OK(partitioner->FindPartition(paver::Partition::kZirconR, &fd));
    ASSERT_OK(partitioner->FindPartition(paver::Partition::kVbMetaA, &fd));
    ASSERT_OK(partitioner->FindPartition(paver::Partition::kVbMetaB, &fd));
    ASSERT_OK(partitioner->FindPartition(paver::Partition::kFuchsiaVolumeManager, &fd));
}

TEST(FixedDevicePartitionerTests, GetBlockSizeTest) {
    IsolatedDevmgr devmgr;
    CreateIsolatedDevmgr(&devmgr);

    fbl::unique_ptr<BlockDevice> fvm, zircon_a, zircon_b, zircon_r, vbmeta_a, vbmeta_b;
    ASSERT_NO_FATAL_FAILURES(BlockDevice::Create(devmgr.devfs_root(), kZirconAType, &zircon_a));
    ASSERT_NO_FATAL_FAILURES(BlockDevice::Create(devmgr.devfs_root(), kZirconBType, &zircon_b));
    ASSERT_NO_FATAL_FAILURES(BlockDevice::Create(devmgr.devfs_root(), kZirconRType, &zircon_r));
    ASSERT_NO_FATAL_FAILURES(BlockDevice::Create(devmgr.devfs_root(), kVbMetaAType, &vbmeta_a));
    ASSERT_NO_FATAL_FAILURES(BlockDevice::Create(devmgr.devfs_root(), kVbMetaBType, &vbmeta_b));
    ASSERT_NO_FATAL_FAILURES(BlockDevice::Create(devmgr.devfs_root(), kFvmType, &fvm));

    auto partitioner = paver::DevicePartitioner::Create(devmgr.devfs_root().duplicate(),
                                                        paver::Arch::kArm64);
    ASSERT_NE(partitioner.get(), nullptr);

    fbl::unique_fd fd;
    uint32_t block_size;
    ASSERT_OK(partitioner->FindPartition(paver::Partition::kZirconA, &fd));
    ASSERT_OK(partitioner->GetBlockSize(fd, &block_size));
    ASSERT_EQ(block_size, kBlockSize);
    ASSERT_OK(partitioner->FindPartition(paver::Partition::kZirconB, &fd));
    ASSERT_OK(partitioner->GetBlockSize(fd, &block_size));
    ASSERT_EQ(block_size, kBlockSize);
    ASSERT_OK(partitioner->FindPartition(paver::Partition::kZirconR, &fd));
    ASSERT_OK(partitioner->GetBlockSize(fd, &block_size));
    ASSERT_EQ(block_size, kBlockSize);
    ASSERT_OK(partitioner->FindPartition(paver::Partition::kVbMetaA, &fd));
    ASSERT_OK(partitioner->GetBlockSize(fd, &block_size));
    ASSERT_EQ(block_size, kBlockSize);
    ASSERT_OK(partitioner->FindPartition(paver::Partition::kVbMetaB, &fd));
    ASSERT_OK(partitioner->GetBlockSize(fd, &block_size));
    ASSERT_EQ(block_size, kBlockSize);
    ASSERT_OK(partitioner->FindPartition(paver::Partition::kFuchsiaVolumeManager, &fd));
    ASSERT_OK(partitioner->GetBlockSize(fd, &block_size));
    ASSERT_EQ(block_size, kBlockSize);
}

TEST(SkipBlockDevicePartitionerTests, UseSkipBlockInterfaceTest) {
    fbl::unique_ptr<SkipBlockDevice> device;
    SkipBlockDevice::Create(kNandInfo, &device);

    fbl::unique_ptr<paver::DevicePartitioner> partitioner;
    ASSERT_EQ(paver::SkipBlockDevicePartitioner::Initialize(device->devfs_root(), &partitioner),
              ZX_OK);
    ASSERT_TRUE(partitioner->UseSkipBlockInterface());
}

TEST(SkipBlockDevicePartitionerTests, ChooseSkipBlockPartitioner) {
    fbl::unique_ptr<SkipBlockDevice> device;
    SkipBlockDevice::Create(kNandInfo, &device);
    auto devfs_root = device->devfs_root();
    fbl::unique_ptr<BlockDevice> zircon_a;
    ASSERT_NO_FATAL_FAILURES(BlockDevice::Create(devfs_root, kZirconAType, &zircon_a));

    auto partitioner = paver::DevicePartitioner::Create(std::move(devfs_root),
                                                        paver::Arch::kArm64);
    ASSERT_NE(partitioner.get(), nullptr);
    ASSERT_TRUE(partitioner->UseSkipBlockInterface());
}

TEST(SkipBlockDevicePartitionerTests, AddPartitionTest) {
    fbl::unique_ptr<SkipBlockDevice> device;
    SkipBlockDevice::Create(kNandInfo, &device);

    fbl::unique_ptr<paver::DevicePartitioner> partitioner;
    ASSERT_EQ(paver::SkipBlockDevicePartitioner::Initialize(device->devfs_root(), &partitioner),
              ZX_OK);
    ASSERT_EQ(partitioner->AddPartition(paver::Partition::kZirconB, nullptr), ZX_ERR_NOT_SUPPORTED);
}

TEST(SkipBlockDevicePartitionerTests, WipeFvmTest) {
    fbl::unique_ptr<SkipBlockDevice> device;
    SkipBlockDevice::Create(kNandInfo, &device);

    fbl::unique_ptr<paver::DevicePartitioner> partitioner;
    ASSERT_EQ(paver::SkipBlockDevicePartitioner::Initialize(device->devfs_root(), &partitioner),
              ZX_OK);
    ASSERT_OK(partitioner->WipeFvm());
}

TEST(SkipBlockDevicePartitionerTests, FinalizePartitionTest) {
    fbl::unique_ptr<SkipBlockDevice> device;
    SkipBlockDevice::Create(kNandInfo, &device);

    fbl::unique_ptr<paver::DevicePartitioner> partitioner;
    ASSERT_EQ(paver::SkipBlockDevicePartitioner::Initialize(device->devfs_root(), &partitioner),
              ZX_OK);

    ASSERT_OK(partitioner->FinalizePartition(paver::Partition::kBootloader));
    ASSERT_OK(partitioner->FinalizePartition(paver::Partition::kZirconA));
    ASSERT_OK(partitioner->FinalizePartition(paver::Partition::kZirconB));
    ASSERT_OK(partitioner->FinalizePartition(paver::Partition::kZirconR));
    ASSERT_OK(partitioner->FinalizePartition(paver::Partition::kVbMetaA));
    ASSERT_OK(partitioner->FinalizePartition(paver::Partition::kVbMetaB));
}

TEST(SkipBlockDevicePartitionerTests, FindPartitionTest) {
    fbl::unique_ptr<SkipBlockDevice> device;
    SkipBlockDevice::Create(kNandInfo, &device);
    auto devfs_root = device->devfs_root();
    fbl::unique_ptr<BlockDevice> fvm;
    ASSERT_NO_FATAL_FAILURES(BlockDevice::Create(devfs_root, kFvmType, &fvm));

    fbl::unique_ptr<paver::DevicePartitioner> partitioner;
    ASSERT_EQ(paver::SkipBlockDevicePartitioner::Initialize(std::move(devfs_root), &partitioner),
              ZX_OK);

    fbl::unique_fd fd;
    ASSERT_OK(partitioner->FindPartition(paver::Partition::kBootloader, &fd));
    ASSERT_OK(partitioner->FindPartition(paver::Partition::kZirconA, &fd));
    ASSERT_OK(partitioner->FindPartition(paver::Partition::kZirconB, &fd));
    ASSERT_OK(partitioner->FindPartition(paver::Partition::kZirconR, &fd));
    ASSERT_OK(partitioner->FindPartition(paver::Partition::kVbMetaA, &fd));
    ASSERT_OK(partitioner->FindPartition(paver::Partition::kVbMetaB, &fd));

    ASSERT_OK(partitioner->FindPartition(paver::Partition::kFuchsiaVolumeManager, &fd));
}

TEST(SkipBlockDevicePartitionerTests, GetBlockSizeTest) {
    fbl::unique_ptr<SkipBlockDevice> device;
    SkipBlockDevice::Create(kNandInfo, &device);
    auto devfs_root = device->devfs_root();
    fbl::unique_ptr<BlockDevice> fvm;
    ASSERT_NO_FATAL_FAILURES(BlockDevice::Create(devfs_root, kFvmType, &fvm));

    fbl::unique_ptr<paver::DevicePartitioner> partitioner;
    ASSERT_EQ(paver::SkipBlockDevicePartitioner::Initialize(std::move(devfs_root), &partitioner),
              ZX_OK);

    fbl::unique_fd fd;
    uint32_t block_size;
    ASSERT_OK(partitioner->FindPartition(paver::Partition::kBootloader, &fd));
    ASSERT_OK(partitioner->GetBlockSize(fd, &block_size));
    ASSERT_EQ(block_size, kPageSize * kPagesPerBlock);
    ASSERT_OK(partitioner->FindPartition(paver::Partition::kZirconA, &fd));
    ASSERT_OK(partitioner->GetBlockSize(fd, &block_size));
    ASSERT_EQ(block_size, kPageSize * kPagesPerBlock);
    ASSERT_OK(partitioner->FindPartition(paver::Partition::kZirconB, &fd));
    ASSERT_OK(partitioner->GetBlockSize(fd, &block_size));
    ASSERT_EQ(block_size, kPageSize * kPagesPerBlock);
    ASSERT_OK(partitioner->FindPartition(paver::Partition::kZirconR, &fd));
    ASSERT_OK(partitioner->GetBlockSize(fd, &block_size));
    ASSERT_EQ(block_size, kPageSize * kPagesPerBlock);
    ASSERT_OK(partitioner->FindPartition(paver::Partition::kVbMetaA, &fd));
    ASSERT_OK(partitioner->GetBlockSize(fd, &block_size));
    ASSERT_EQ(block_size, kPageSize * kPagesPerBlock);
    ASSERT_OK(partitioner->FindPartition(paver::Partition::kVbMetaB, &fd));
    ASSERT_OK(partitioner->GetBlockSize(fd, &block_size));
    ASSERT_EQ(block_size, kPageSize * kPagesPerBlock);

    ASSERT_OK(partitioner->FindPartition(paver::Partition::kFuchsiaVolumeManager, &fd));
    ASSERT_OK(partitioner->GetBlockSize(fd, &block_size));
    ASSERT_EQ(block_size, kBlockSize);
}
