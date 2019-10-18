// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <lib/devmgr-integration-test/fixture.h>

#include <string>

#include <fbl/macros.h>
#include <fs-management/fvm.h>
#include <fs-management/mount.h>
#include <ramdevice-client/ramdisk.h>
#include <zxtest/zxtest.h>

// Simple wrapper around a ramdisk.
class RamDisk {
 public:
  RamDisk(const fbl::unique_fd& devfs_root, uint32_t page_size, uint32_t num_pages);
  ~RamDisk();

  const char* path() const { return path_.c_str(); }
  uint32_t page_size() const { return page_size_; }

  // Expose the ramdisk client functionality.
  zx_status_t SleepAfter(uint32_t block_count) const;
  zx_status_t WakeUp() const;
  zx_status_t GetBlockCounts(ramdisk_block_write_counts_t* counts) const;

  DISALLOW_COPY_ASSIGN_AND_MOVE(RamDisk);

 private:
  uint32_t page_size_;
  uint32_t num_pages_;
  ramdisk_client_t* ramdisk_ = nullptr;
  std::string path_;
};

// Process-wide environment for tests. This takes care of dealing with a
// physical or emulated block device for the tests in addition to configuration
// parameters.
class Environment : public zxtest::Environment {
 public:
  struct TestConfig {
    const char* path;  // Path to an existing device.
    const char* mount_path;
    disk_format_type format_type;
    bool show_help;
    bool use_journal = true;

    // Updates the configuration with options from the command line.
    // Returns false as soon as an option is not recognized.
    bool GetOptions(int argc, char** argv);

    // Returns the help message.
    const char* HelpMessage() const;
  };

  explicit Environment(const TestConfig& config) : config_(config) {}
  ~Environment() {}

  // zxtest::Environment interface:
  void SetUp() override;
  void TearDown() override;

  bool use_journal() const { return config_.use_journal; }

  disk_format_type format_type() const { return config_.format_type; }

  const char* mount_path() const { return config_.mount_path; }

  uint64_t disk_size() const { return block_size_ * block_count_; }

  const char* device_path() const { return path_.c_str(); }

  // Returns the path of the underlying device with the caveat that if the test
  // is using a ramdisk, the returned path is not usable to access the device
  // because it will not be rooted on the correct device manager. This only
  // makes sense when comparing against a path provided by the filesystem.
  const char* GetRelativeDevicePath() const;

  const RamDisk* ramdisk() const { return ramdisk_.get(); }

  const fbl::unique_fd& devfs_root() const { return devmgr_.devfs_root(); }

  DISALLOW_COPY_ASSIGN_AND_MOVE(Environment);

 private:
  bool OpenDevice(const char* path);
  void CreateDevmgr();

  TestConfig config_;

  devmgr_integration_test::IsolatedDevmgr devmgr_;
  std::unique_ptr<RamDisk> ramdisk_;
  std::string path_;

  uint32_t block_size_ = 512;
  uint64_t block_count_ = 1 << 19;  // TODO(ZX-4203): Reduce this value.
};

extern Environment* g_environment;
