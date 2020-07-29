// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/storage/fs_test/fs_test.h"

#include <errno.h>
#include <fuchsia/fs/cpp/fidl.h>
#include <fuchsia/io/llcpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/fdio/cpp/caller.h>
#include <lib/fdio/directory.h>
#include <lib/memfs/memfs.h>
#include <lib/sync/completion.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/zx/channel.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zircon/errors.h>

#include <utility>

#include <fbl/unique_fd.h>
#include <fs-management/admin.h>
#include <fs-management/format.h>
#include <fs-management/launch.h>
#include <fs-management/mount.h>

#include "src/lib/isolated_devmgr/v2_component/fvm.h"

namespace fs_test {

namespace fio = ::llcpp::fuchsia::io;

// Creates a ram-disk with an optional FVM partition. Returns the ram-disk and the device path.
static zx::status<std::pair<isolated_devmgr::RamDisk, std::string>> CreateRamDisk(
    const TestFilesystemOptions& options) {
  // Create a ram-disk.
  auto ram_disk_or =
      isolated_devmgr::RamDisk::Create(options.device_block_size, options.device_block_count);
  if (ram_disk_or.is_error()) {
    return ram_disk_or.take_error();
  }

  // Create an FVM partition if requested.
  std::string device_path;
  if (options.use_fvm) {
    auto fvm_partition_or =
        isolated_devmgr::CreateFvmPartition(ram_disk_or.value().path(), options.fvm_slice_size);
    if (fvm_partition_or.is_error()) {
      return fvm_partition_or.take_error();
    }
    device_path = fvm_partition_or.value();
  } else {
    device_path = ram_disk_or.value().path();
  }

  return zx::ok(std::make_pair(std::move(ram_disk_or).value(), device_path));
}

// A wrapper around fs-management that can be used by filesytems if they so wish.
static zx::status<> FsMount(const std::string& device_path, const std::string& mount_path,
                            disk_format_t format, const mount_options_t& mount_options,
                            zx::channel* outgoing_directory = nullptr) {
  auto fd = fbl::unique_fd(open(device_path.c_str(), O_RDWR));
  if (!fd) {
    FX_LOGS(ERROR) << "Could not open device: " << device_path << ": errno=" << errno;
    return zx::error(ZX_ERR_BAD_STATE);
  }

  mount_options_t options = mount_options;
  options.register_fs = false;
  if (outgoing_directory) {
    zx::channel server;
    auto status = zx::make_status(zx::channel::create(0, outgoing_directory, &server));
    if (status.is_error()) {
      FX_LOGS(ERROR) << "Unable to create channel for outgoing directory: "
                     << status.status_string();
      return status;
    }
    options.outgoing_directory.client = outgoing_directory->get();
    options.outgoing_directory.server = server.release();
  }

  // Uncomment the following line to force an fsck at the end of every transaction (where
  // supported).
  // options.fsck_after_every_transaction = true;

  // |fd| is consumed by mount.
  auto status = zx::make_status(
      mount(fd.release(), mount_path.c_str(), format, &options, launch_stdio_async));
  if (status.is_error()) {
    FX_LOGS(ERROR) << "Could not mount " << disk_format_string(format)
                   << " file system: " << status.status_string();
    return status;
  }
  return zx::ok();
}

TestFilesystemOptions TestFilesystemOptions::DefaultMinfs() {
  return TestFilesystemOptions{.description = "MinfsWithFvm",
                               .use_fvm = true,
                               .device_block_size = 512,
                               .device_block_count = 131'072,
                               .fvm_slice_size = 8 * 1'048'576,
                               .filesystem = &MinfsFilesystem::SharedInstance()};
}

TestFilesystemOptions TestFilesystemOptions::MinfsWithoutFvm() {
  TestFilesystemOptions minfs_with_no_fvm = TestFilesystemOptions::DefaultMinfs();
  minfs_with_no_fvm.description = "MinfsWithoutFvm";
  minfs_with_no_fvm.use_fvm = false;
  return minfs_with_no_fvm;
}

TestFilesystemOptions TestFilesystemOptions::DefaultMemfs() {
  return TestFilesystemOptions{.description = "Memfs",
                               .filesystem = &MemfsFilesystem::SharedInstance()};
}

TestFilesystemOptions TestFilesystemOptions::DefaultFatfs() {
  return TestFilesystemOptions{.description = "Fatfs",
                               .use_fvm = false,
                               .device_block_size = 512,
                               .device_block_count = 196'608,
                               .fvm_slice_size = 1'048'576,
                               .filesystem = &FatFilesystem::SharedInstance()};
}

std::ostream& operator<<(std::ostream& out, const TestFilesystemOptions& options) {
  return out << options.description;
}

std::vector<TestFilesystemOptions> AllTestFilesystems() {
  return std::vector<TestFilesystemOptions>{TestFilesystemOptions::DefaultMinfs(),
                                            TestFilesystemOptions::MinfsWithoutFvm(),
                                            TestFilesystemOptions::DefaultMemfs(),
#ifdef TEST_FATFS
                                            TestFilesystemOptions::DefaultFatfs()
#endif
  };
}

std::vector<TestFilesystemOptions> MapAndFilterAllTestFilesystems(
    std::function<std::optional<TestFilesystemOptions>(const TestFilesystemOptions&)>
        map_and_filter) {
  std::vector<TestFilesystemOptions> results;
  for (const TestFilesystemOptions& options : AllTestFilesystems()) {
    auto r = map_and_filter(options);
    if (r) {
      results.push_back(*std::move(r));
    }
  }
  return results;
}

zx::status<> Filesystem::Format(const std::string& device_path, disk_format_t format) {
  mkfs_options_t options = default_mkfs_options;
  options.sectors_per_cluster = 2;  // 1 KiB cluster size
  auto status = zx::make_status(mkfs(device_path.c_str(), format, launch_stdio_sync, &options));
  if (status.is_error()) {
    FX_LOGS(ERROR) << "Could not format " << disk_format_string(format)
                   << " file system: " << status.status_string();
    return status;
  }
  return zx::ok();
}

// -- Minfs --

class MinfsInstance : public FilesystemInstance {
 public:
  MinfsInstance(isolated_devmgr::RamDisk ram_disk, const std::string& device_path)
      : ram_disk_(std::move(ram_disk)), device_path_(device_path) {}

  zx::status<> Mount(const std::string& mount_path) override {
    return FsMount(device_path_, mount_path, DISK_FORMAT_MINFS, default_mount_options);
  }

  zx::status<> Unmount(const std::string& mount_path) override {
    return zx::make_status(umount(mount_path.c_str()));
  }

  zx::status<> Fsck() override {
    fsck_options_t options{
        .verbose = false,
        .never_modify = true,
        .always_modify = false,
        .force = true,
        .apply_journal = false,
    };
    return zx::make_status(
        fsck(device_path_.c_str(), DISK_FORMAT_MINFS, &options, launch_stdio_sync));
  }

  isolated_devmgr::RamDisk* GetRamDisk() override { return &ram_disk_; }

 private:
  isolated_devmgr::RamDisk ram_disk_;
  std::string device_path_;
};

zx::status<std::unique_ptr<FilesystemInstance>> MinfsFilesystem::Make(
    const TestFilesystemOptions& options) const {
  auto ram_disk_or = CreateRamDisk(options);
  if (ram_disk_or.is_error()) {
    return ram_disk_or.take_error();
  }
  auto [ram_disk, device_path] = std::move(ram_disk_or).value();
  zx::status<> status = Filesystem::Format(device_path, DISK_FORMAT_MINFS);
  if (status.is_error()) {
    return status.take_error();
  }
  return zx::ok(std::make_unique<MinfsInstance>(std::move(ram_disk), device_path));
}

// -- Memfs --

class MemfsInstance : public FilesystemInstance {
 public:
  MemfsInstance() : loop_(&kAsyncLoopConfigNeverAttachToThread) {
    FX_CHECK(loop_.StartThread() == ZX_OK);
  }
  ~MemfsInstance() override {
    if (fs_) {
      sync_completion_t sync;
      memfs_free_filesystem(fs_, &sync);
      FX_CHECK(sync_completion_wait(&sync, zx::duration::infinite().get()) == ZX_OK);
    }
  }
  zx::status<> Format() {
    return zx::make_status(
        memfs_create_filesystem(loop_.dispatcher(), &fs_, root_.reset_and_get_address()));
  }

  zx::status<> Mount(const std::string& mount_path) override {
    if (!root_) {
      // Already mounted.
      return zx::error(ZX_ERR_BAD_STATE);
    }
    auto status = zx::make_status(mount_root_handle(root_.release(), mount_path.c_str()));
    if (status.is_error())
      FX_LOGS(ERROR) << "Unable to mount: " << status.status_string();
    return status;
  }

  zx::status<> Unmount(const std::string& mount_path) override {
    // We can't use fs-management here because it also shuts down the file system, which we don't
    // want to do because then we wouldn't be able to remount. O_ADMIN and O_NOREMOTE are not
    // available in the SDK, which makes detaching the remote mount ourselves difficult.  So, for
    // now, just do nothing; we don't really need to test this.
    return zx::ok();
  }

  zx::status<> Fsck() override { return zx::ok(); }

 private:
  async::Loop loop_;
  memfs_filesystem_t* fs_ = nullptr;
  zx::channel root_;  // Not valid after mounted.
};

zx::status<std::unique_ptr<FilesystemInstance>> MemfsFilesystem::Make(
    const TestFilesystemOptions& options) const {
  auto instance = std::make_unique<MemfsInstance>();
  zx::status<> status = instance->Format();
  if (status.is_error()) {
    return status.take_error();
  }
  return zx::ok(std::move(instance));
}

// -- Fatfs --

class FatfsInstance : public FilesystemInstance {
 public:
  FatfsInstance(isolated_devmgr::RamDisk ram_disk, const std::string& device_path)
      : ram_disk_(std::move(ram_disk)), device_path_(device_path) {}

  zx::status<> Mount(const std::string& mount_path) override {
    mount_options_t options = default_mount_options;
    // Fatfs doesn't support DirectoryAdmin.
    options.admin = false;
    return FsMount(device_path_, mount_path, DISK_FORMAT_FAT, options, &outgoing_directory_);
  }

  zx::status<> Unmount(const std::string& mount_path) override {
    // O_ADMIN & O_NO_REMOTE are not part of the SDK and O_ADMIN, at least, is deprecated, so for
    // now, we hard code their values until we get around to fixing fs-management. fatfs doesn't
    // support O_ADMIN.

    // First detach the node.
    constexpr int kAdmin = 0x0000'0004;
    constexpr int kNoRemote = 0x0020'0000;
    auto fd = fbl::unique_fd(open(mount_path.c_str(), O_DIRECTORY | kNoRemote | kAdmin));
    if (!fd) {
      FX_LOGS(ERROR) << "Unable to open mount point for unmount: " << strerror(errno);
      return zx::error(ZX_ERR_IO);
    }
    fdio_cpp::FdioCaller caller(std::move(fd));
    auto response = fio::DirectoryAdmin::Call::UnmountNode(caller.channel());
    caller.release().release();
    if (!response.ok()) {
      auto status = zx::make_status(response.status());
      FX_LOGS(ERROR) << "UnmountNode failed with fidl error: " << status.status_string();
      return status;
    }
    if (response.value().s != ZX_OK) {
      auto status = zx::make_status(response.value().s);
      FX_LOGS(ERROR) << "UnmountNode failed: " << status.status_string();
      return status;
    }

    // Now shut down the filesystem.
    fidl::SynchronousInterfacePtr<fuchsia::fs::Admin> admin;
    std::string service_name = std::string("svc/") + fuchsia::fs::Admin::Name_;
    auto status = zx::make_status(fdio_service_connect_at(
        outgoing_directory_.get(), service_name.c_str(), admin.NewRequest().TakeChannel().get()));
    if (status.is_error()) {
      FX_LOGS(ERROR) << "Unable to connect to admin service: " << status.status_string();
      return status;
    }
    status = zx::make_status(admin->Shutdown());
    if (status.is_error()) {
      FX_LOGS(ERROR) << "Shut down failed: " << status.status_string();
      return status;
    }
    outgoing_directory_.reset();

    return zx::ok();
  }

  zx::status<> Fsck() override {
    fsck_options_t options{
        .verbose = false,
        .never_modify = true,
        .always_modify = false,
        .force = true,
        .apply_journal = false,
    };
    return zx::make_status(
        fsck(device_path_.c_str(), DISK_FORMAT_FAT, &options, launch_stdio_sync));
  }

 private:
  isolated_devmgr::RamDisk ram_disk_;
  std::string device_path_;
  zx::channel outgoing_directory_;
};

zx::status<std::unique_ptr<FilesystemInstance>> FatFilesystem::Make(
    const TestFilesystemOptions& options) const {
  auto ram_disk_or = CreateRamDisk(options);
  if (ram_disk_or.is_error()) {
    return ram_disk_or.take_error();
  }
  auto [ram_disk, device_path] = std::move(ram_disk_or).value();
  zx::status<> status = Filesystem::Format(device_path, DISK_FORMAT_FAT);
  if (status.is_error()) {
    return status.take_error();
  }
  return zx::ok(std::make_unique<FatfsInstance>(std::move(ram_disk), device_path));
}

// --

zx::status<TestFilesystem> TestFilesystem::Create(const TestFilesystemOptions& options) {
  // Make a file system.
  auto instance_or = options.filesystem->Make(options);
  if (instance_or.is_error()) {
    return instance_or.take_error();
  }

  // Mount the file system.
  char mount_path_c_str[] = "/tmp/fs_test.XXXXXX";
  if (mkdtemp(mount_path_c_str) == nullptr) {
    FX_LOGS(ERROR) << "Unable to create mount point: " << errno;
    return zx::error(ZX_ERR_BAD_STATE);
  }
  TestFilesystem filesystem(options, std::move(instance_or).value(), mount_path_c_str);
  auto status = filesystem.Mount();
  if (status.is_error()) {
    return status.take_error();
  }
  return zx::ok(std::move(filesystem));
}

TestFilesystem::~TestFilesystem() {
  if (filesystem_) {
    if (mounted_) {
      auto status = Unmount();
      if (status.is_error()) {
        FX_LOGS(WARNING) << "Failed to unmount: " << status.status_string();
      }
    }
    rmdir(mount_path_.c_str());
  }
}

zx::status<> TestFilesystem::Mount() {
  auto status = filesystem_->Mount(mount_path_);
  if (status.is_ok()) {
    mounted_ = true;
  }
  return status;
}

zx::status<> TestFilesystem::Unmount() {
  if (!filesystem_) {
    return zx::ok();
  }
  auto status = filesystem_->Unmount(mount_path_.c_str());
  if (status.is_ok()) {
    mounted_ = false;
  }
  return status;
}

zx::status<> TestFilesystem::Fsck() { return filesystem_->Fsck(); }

}  // namespace fs_test
