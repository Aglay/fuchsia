// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fbl/algorithm.h>
#include <fbl/auto_call.h>
#include <fbl/string_buffer.h>
#include <fbl/unique_fd.h>
#include <fs-management/mount.h>
#include <fuchsia/device/c/fidl.h>
#include <fuchsia/hardware/block/c/fidl.h>
#include <fuchsia/hardware/block/partition/c/fidl.h>
#include <gpt/gpt.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/fdio.h>
#include <lib/fdio/unsafe.h>
#include <lib/fdio/watcher.h>
#include <lib/fzl/fdio.h>
#include <lib/fzl/time.h>
#include <lib/zx/channel.h>
#include <lib/zx/process.h>
#include <lib/zx/time.h>
#include <loader-service/loader-service.h>
#include <minfs/minfs.h>
#include <ramdevice-client/ramdisk.h>
#include <zircon/device/block.h>
#include <zircon/processargs.h>
#include <zircon/status.h>
#include <zircon/syscalls.h>
#include <zxcrypt/fdio-volume.h>

#include <utility>

#include "fs-manager.h"

namespace devmgr {
namespace {

class BlockWatcher {
public:
    BlockWatcher(fbl::unique_ptr<FsManager> fshost, bool netboot)
        : fshost_(std::move(fshost)), netboot_(netboot) {}

    void FuchsiaStart() const { fshost_->FuchsiaStart(); }

    zx_status_t InstallFs(const char* path, zx::channel h) {
        return fshost_->InstallFs(path, std::move(h));
    }

    bool Netbooting() const { return netboot_; }

    // Optionally checks the filesystem stored on the device at |device_path|,
    // if "zircon.system.filesystem-check" is set.
    zx_status_t CheckFilesystem(const char* device_path, disk_format_t df,
                                const fsck_options_t* options) const;

    // Attempts to mount a block device backed by |fd| to "/data".
    // Fails if already mounted.
    zx_status_t MountData(fbl::unique_fd fd, mount_options_t* options);

    // Attempts to mount a block device backed by |fd| to "/install".
    // Fails if already mounted.
    zx_status_t MountInstall(fbl::unique_fd fd, mount_options_t* options);

    // Attempts to mount a block device backed by |fd| to "/blob".
    // Fails if already mounted.
    zx_status_t MountBlob(fbl::unique_fd fd, mount_options_t* options);

private:
    fbl::unique_ptr<FsManager> fshost_;
    bool netboot_ = false;
    bool data_mounted_ = false;
    bool install_mounted_ = false;
    bool blob_mounted_ = false;
};

void pkgfs_finish(BlockWatcher* watcher, zx::process proc, zx::channel pkgfs_root) {
    auto deadline = zx::deadline_after(zx::sec(5));
    zx_signals_t observed;
    zx_status_t status =
        proc.wait_one(ZX_USER_SIGNAL_0 | ZX_PROCESS_TERMINATED, deadline, &observed);
    if (status != ZX_OK) {
        printf("fshost: pkgfs did not signal completion: %d (%s)\n", status,
               zx_status_get_string(status));
        return;
    }
    if (!(observed & ZX_USER_SIGNAL_0)) {
        printf("fshost: pkgfs terminated prematurely\n");
        return;
    }
    // re-export /pkgfs/system as /system
    zx::channel system_channel, system_req;
    if (zx::channel::create(0, &system_channel, &system_req) != ZX_OK) {
        return;
    }
    if (fdio_open_at(pkgfs_root.get(), "system", FS_READONLY_DIR_FLAGS,
                     system_req.release()) != ZX_OK) {
        return;
    }
    // re-export /pkgfs/packages/shell-commands/0/bin as /bin
    zx::channel bin_chan, bin_req;
    if (zx::channel::create(0, &bin_chan, &bin_req) != ZX_OK) {
        return;
    }
    if (fdio_open_at(pkgfs_root.get(), "packages/shell-commands/0/bin", FS_READONLY_DIR_FLAGS,
                     bin_req.release()) != ZX_OK) {
        // non-fatal.
        printf("fshost: failed to install /bin (could not open shell-commands)\n");
    }

    if (watcher->InstallFs("/pkgfs", std::move(pkgfs_root)) != ZX_OK) {
        printf("fshost: failed to install /pkgfs\n");
        return;
    }

    if (watcher->InstallFs("/system", std::move(system_channel)) != ZX_OK) {
        printf("fshost: failed to install /system\n");
        return;
    }

    // as above, failure of /bin export is non-fatal.
    if (watcher->InstallFs("/bin", std::move(bin_chan)) != ZX_OK) {
        printf("fshost: failed to install /bin\n");
    }

    // start the appmgr
    watcher->FuchsiaStart();
}

// Launching pkgfs uses its own loader service and command lookup to run out of
// the blobfs without any real filesystem.  Files are found by
// getenv("zircon.system.pkgfs.file.PATH") returning a blob content ID.
// That is, a manifest of name->blob is embedded in /boot/config/devmgr.
zx_status_t pkgfs_ldsvc_load_blob(void* ctx, const char* prefix, const char* name,
                                  zx_handle_t* vmo) {
    const int fs_blob_fd = static_cast<int>(reinterpret_cast<intptr_t>(ctx));
    char key[256];
    if (snprintf(key, sizeof(key), "zircon.system.pkgfs.file.%s%s", prefix, name) >=
        (int)sizeof(key)) {
        return ZX_ERR_BAD_PATH;
    }
    const char* blob = getenv(key);
    if (blob == nullptr) {
        return ZX_ERR_NOT_FOUND;
    }
    int fd = openat(fs_blob_fd, blob, O_RDONLY);
    if (fd < 0) {
        return ZX_ERR_NOT_FOUND;
    }

    zx::vmo nonexec_vmo;
    zx::vmo exec_vmo;
    zx_status_t status = fdio_get_vmo_clone(fd, nonexec_vmo.reset_and_get_address());
    close(fd);
    if (status != ZX_OK) {
        return status;
    }
    status = nonexec_vmo.replace_as_executable(zx::handle(), &exec_vmo);
    if (status != ZX_OK) {
        return status;
    }
    status = zx_object_set_property(exec_vmo.get(), ZX_PROP_NAME, key, strlen(key));
    if (status != ZX_OK) {
        return status;
    }

    *vmo = exec_vmo.release();
    return ZX_OK;
}

zx_status_t pkgfs_ldsvc_load_object(void* ctx, const char* name, zx_handle_t* vmo) {
    return pkgfs_ldsvc_load_blob(ctx, "lib/", name, vmo);
}

zx_status_t pkgfs_ldsvc_load_abspath(void* ctx, const char* name, zx_handle_t* vmo) {
    return pkgfs_ldsvc_load_blob(ctx, "", name + 1, vmo);
}

zx_status_t pkgfs_ldsvc_publish_data_sink(void* ctx, const char* name, zx_handle_t vmo) {
    zx_handle_close(vmo);
    return ZX_ERR_NOT_SUPPORTED;
}

void pkgfs_ldsvc_finalizer(void* ctx) {
    close(static_cast<int>(reinterpret_cast<intptr_t>(ctx)));
}

const loader_service_ops_t pkgfs_ldsvc_ops = {
    .load_object = pkgfs_ldsvc_load_object,
    .load_abspath = pkgfs_ldsvc_load_abspath,
    .publish_data_sink = pkgfs_ldsvc_publish_data_sink,
    .finalizer = pkgfs_ldsvc_finalizer,
};

// Create a local loader service with a fixed mapping of names to blobs.
zx_status_t pkgfs_ldsvc_start(fbl::unique_fd fs_blob_fd, zx::channel* ldsvc) {
    loader_service_t* service;
    zx_status_t status =
        loader_service_create(nullptr, &pkgfs_ldsvc_ops,
                              reinterpret_cast<void*>(static_cast<intptr_t>(fs_blob_fd.get())),
                              &service);
    if (status != ZX_OK) {
        printf("fshost: cannot create pkgfs loader service: %d (%s)\n", status,
               zx_status_get_string(status));
        return status;
    }
    // The loader service now owns this FD
    __UNUSED auto fd = fs_blob_fd.release();

    status = loader_service_connect(service, ldsvc->reset_and_get_address());
    loader_service_release(service);
    if (status != ZX_OK) {
        printf("fshost: cannot connect pkgfs loader service: %d (%s)\n", status,
               zx_status_get_string(status));
    }
    return status;
}

bool pkgfs_launch(BlockWatcher* watcher) {
    const char* cmd = getenv("zircon.system.pkgfs.cmd");
    if (cmd == nullptr) {
        return false;
    }

    fbl::unique_fd fs_blob_fd(open("/fs/blob", O_RDONLY | O_DIRECTORY));
    if (!fs_blob_fd) {
        printf("fshost: open(/fs/blob): %m\n");
        return false;
    }

    zx::channel h0, h1;
    zx_status_t status = zx::channel::create(0, &h0, &h1);
    if (status != ZX_OK) {
        printf("fshost: cannot create pkgfs root channel: %d (%s)\n", status,
               zx_status_get_string(status));
        return false;
    }

    auto args = ArgumentVector::FromCmdline(cmd);
    auto argv = args.argv();
    // Remove leading slashes before asking pkgfs_ldsvc_load_blob to load the
    // file.
    const char* file = argv[0];
    while (file[0] == '/') {
        ++file;
    }
    zx::vmo executable;
    status = pkgfs_ldsvc_load_blob(reinterpret_cast<void*>(static_cast<intptr_t>(fs_blob_fd.get())),
                                   "", argv[0], executable.reset_and_get_address());
    if (status != ZX_OK) {
        printf("fshost: cannot load pkgfs executable: %d (%s)\n", status,
               zx_status_get_string(status));
        return false;
    }

    zx::channel loader;
    status = pkgfs_ldsvc_start(std::move(fs_blob_fd), &loader);
    if (status != ZX_OK) {
        printf("fshost: cannot pkgfs loader: %d (%s)\n", status, zx_status_get_string(status));
        return false;
    }

    const zx_handle_t raw_h1 = h1.release();
    zx::process proc;
    args.Print("fshost");
    status = devmgr_launch_with_loader(*zx::job::default_job(), "pkgfs",
                                       std::move(executable), std::move(loader),
                                       argv, nullptr, -1, &raw_h1,
                                       (const uint32_t[]){PA_HND(PA_USER0, 0)}, 1, &proc,
                                       FS_DATA | FS_BLOB | FS_SVC);
    if (status != ZX_OK) {
        printf("fshost: failed to launch %s: %d (%s)\n", cmd, status, zx_status_get_string(status));
        return false;
    }

    pkgfs_finish(watcher, std::move(proc), std::move(h0));
    return true;
}

void LaunchBlobInit(BlockWatcher* watcher) {
    pkgfs_launch(watcher);
}

zx_status_t LaunchBlobfs(int argc, const char** argv, zx_handle_t* hnd, uint32_t* ids,
                          size_t len) {
    return devmgr_launch(*zx::job::default_job(), "blobfs:/blob", argv, nullptr,
                         -1, hnd, ids, len, nullptr, FS_FOR_FSPROC);
}

zx_status_t LaunchMinfs(int argc, const char** argv, zx_handle_t* hnd, uint32_t* ids, size_t len) {
    return devmgr_launch(*zx::job::default_job(), "minfs:/data", argv, nullptr,
                         -1, hnd, ids, len, nullptr, FS_FOR_FSPROC);
}

zx_status_t LaunchFAT(int argc, const char** argv, zx_handle_t* hnd, uint32_t* ids, size_t len) {
    return devmgr_launch(*zx::job::default_job(), "fatfs:/volume", argv, nullptr,
                         -1, hnd, ids, len, nullptr, FS_FOR_FSPROC);
}

zx_status_t BlockWatcher::MountData(fbl::unique_fd fd, mount_options_t* options) {
    if (data_mounted_) {
        return ZX_ERR_ALREADY_BOUND;
    }
    options->wait_until_ready = true;

    zx_status_t status =
        mount(fd.release(), "/fs" PATH_DATA, DISK_FORMAT_MINFS, options, LaunchMinfs);
    if (status != ZX_OK) {
        printf("fshost: failed to mount %s: %s.\n", PATH_DATA, zx_status_get_string(status));
    } else {
        data_mounted_ = true;
    }
    return status;
}

zx_status_t BlockWatcher::MountInstall(fbl::unique_fd fd, mount_options_t* options) {
    if (install_mounted_) {
        return ZX_ERR_ALREADY_BOUND;
    }
    options->readonly = true;
    zx_status_t status =
        mount(fd.release(), "/fs" PATH_INSTALL, DISK_FORMAT_MINFS, options, LaunchMinfs);
    if (status != ZX_OK) {
        printf("fshost: failed to mount %s: %s.\n", PATH_INSTALL, zx_status_get_string(status));
    } else {
        install_mounted_ = true;
    }
    return status;
}

zx_status_t BlockWatcher::MountBlob(fbl::unique_fd fd, mount_options_t* options) {
    if (blob_mounted_) {
        return ZX_ERR_ALREADY_BOUND;
    }
    zx_status_t status =
        mount(fd.release(), "/fs" PATH_BLOB, DISK_FORMAT_BLOBFS, options, LaunchBlobfs);
    if (status != ZX_OK) {
        printf("fshost: failed to mount %s: %s.\n", PATH_BLOB, zx_status_get_string(status));
    } else {
        blob_mounted_ = true;
    }
    return status;
}

zx_status_t BlockWatcher::CheckFilesystem(const char* device_path, disk_format_t df,
                                          const fsck_options_t* options) const {
    if (!getenv_bool("zircon.system.filesystem-check", false)) {
        return ZX_OK;
    }

    // TODO(ZX-3793): Blobfs' consistency checker is too slow to execute on boot.
    // With journaling, it is also unnecessary, but would be a nice mechanism for sanity
    // checking.
    if (df == DISK_FORMAT_BLOBFS) {
        fprintf(stderr, "fshost: Skipping blobfs consistency checker\n");
        return ZX_OK;
    }

    zx::ticks before = zx::ticks::now();
    auto timer = fbl::MakeAutoCall([before]() {
        auto after = zx::ticks::now();
        auto duration = fzl::TicksToNs(after - before);
        printf("fshost: fsck took %" PRId64 ".%" PRId64 " seconds\n", duration.to_secs(),
               duration.to_msecs() % 1000);
    });

    printf("fshost: fsck of %s started\n", disk_format_string_[df]);

    auto launch_fsck = [](int argc, const char** argv, zx_handle_t* hnd, uint32_t* ids,
                          size_t len) {
        zx::process proc;
        zx_status_t status = devmgr_launch(*zx::job::default_job(), "fsck", argv,
                                           nullptr, -1, hnd, ids, len, &proc, FS_FOR_FSPROC);
        if (status != ZX_OK) {
            fprintf(stderr, "fshost: Couldn't launch fsck\n");
            return status;
        }
        status = proc.wait_one(ZX_PROCESS_TERMINATED, zx::time::infinite(), nullptr);
        if (status != ZX_OK) {
            fprintf(stderr, "fshost: Error waiting for fsck to terminate\n");
            return status;
        }

        zx_info_process_t info;
        status = proc.get_info(ZX_INFO_PROCESS, &info, sizeof(info), nullptr, nullptr);
        if (status != ZX_OK) {
            fprintf(stderr, "fshost: Failed to get process info\n");
            return status;
        }

        if (info.return_code != 0) {
            fprintf(stderr, "fshost: Fsck return code: %" PRId64 "\n", info.return_code);
            return ZX_ERR_BAD_STATE;
        }
        return ZX_OK;
    };

    zx_status_t status = fsck(device_path, df, options, launch_fsck);
    if (status != ZX_OK) {
        fprintf(stderr, "--------------------------------------------------------------\n");
        fprintf(stderr, "|                                                             \n");
        fprintf(stderr, "|   WARNING: fshost fsck failure!                             \n");
        fprintf(stderr, "|   Corrupt %s @ %s \n", disk_format_string_[df], device_path);
        fprintf(stderr, "|                                                             \n");
        fprintf(stderr, "|   If your system encountered power-loss due to an unclean   \n");
        fprintf(stderr, "|   shutdown, this error was expected. Journaling in minfs    \n");
        fprintf(stderr, "|   is being tracked by ZX-2093. Re-paving will reset your    \n");
        fprintf(stderr, "|   device.                                                   \n");
        fprintf(stderr, "|                                                             \n");
        fprintf(stderr, "|   If your system was shutdown cleanly (via 'dm poweroff'    \n");
        fprintf(stderr, "|   or an OTA), report this device to the local-storage       \n");
        fprintf(stderr, "|   team. Please file bugs with logs before and after reboot. \n");
        fprintf(stderr, "|   Please use the 'filesystem' and 'minfs' component tag.    \n");
        fprintf(stderr, "|                                                             \n");
        fprintf(stderr, "--------------------------------------------------------------\n");
    } else {
        printf("fshost: fsck of %s completed OK\n", disk_format_string_[df]);
    }
    return status;
}

// Attempt to mount the device pointed to be the file descriptor at a known
// location.
//
// Returns ZX_ERR_ALREADY_BOUND if the device could be mounted, but something
// is already mounted at that location. Returns ZX_ERR_INVALID_ARGS if the
// GUID of the device does not match a known valid one. Returns
// ZX_ERR_NOT_SUPPORTED if the GUID is a system GUID. Returns ZX_OK if an
// attempt to mount is made, without checking mount success.
zx_status_t MountMinfs(BlockWatcher* watcher, fbl::unique_fd fd, mount_options_t* options) {
    fuchsia_hardware_block_partition_GUID type_guid;
    {
        fzl::UnownedFdioCaller disk_connection(fd.get());
        zx::unowned_channel channel(disk_connection.borrow_channel());
        zx_status_t io_status, status;
        io_status = fuchsia_hardware_block_partition_PartitionGetTypeGuid(channel->get(), &status,
                                                                          &type_guid);
        if (io_status != ZX_OK)
            return io_status;
        if (status != ZX_OK)
            return status;
    }

    if (gpt_is_sys_guid(type_guid.value, GPT_GUID_LEN)) {
        return ZX_ERR_NOT_SUPPORTED;
    } else if (gpt_is_data_guid(type_guid.value, GPT_GUID_LEN)) {
        return watcher->MountData(std::move(fd), options);
    } else if (gpt_is_install_guid(type_guid.value, GPT_GUID_LEN)) {
        return watcher->MountInstall(std::move(fd), options);
    }
    printf("fshost: Unrecognized partition GUID for minfs; not mounting\n");
    return ZX_ERR_INVALID_ARGS;
}

#define FVM_DRIVER_LIB "/boot/driver/fvm.so"
#define GPT_DRIVER_LIB "/boot/driver/gpt.so"
#define MBR_DRIVER_LIB "/boot/driver/mbr.so"
#define BOOTPART_DRIVER_LIB "/boot/driver/bootpart.so"
#define ZXCRYPT_DRIVER_LIB "/boot/driver/zxcrypt.so"
#define STRLEN(s) (sizeof(s) / sizeof((s)[0]))

// return value is ignored
int UnsealZxcrypt(void* arg) {
    fbl::unique_ptr<int> fd_ptr(static_cast<int*>(arg));
    fbl::unique_fd fd(*fd_ptr);

    zx_status_t rc;
    fbl::unique_ptr<zxcrypt::FdioVolume> zxcrypt_volume;
    if ((rc = zxcrypt::FdioVolume::Init(std::move(fd), &zxcrypt_volume)) != ZX_OK) {
        printf("fshost: couldn't open zxcrypt fdio volume");
        return ZX_OK;
    }

    zx::channel zxcrypt_volume_manager_chan;
    if ((rc = zxcrypt_volume->OpenManager(zx::sec(2), zxcrypt_volume_manager_chan.reset_and_get_address())) != ZX_OK) {
        printf("fshost: couldn't open zxcrypt manager device");
        return 0;
    }

    zxcrypt::FdioVolumeManager zxcrypt_volume_manager(std::move(zxcrypt_volume_manager_chan));
    uint8_t slot = 0;
    if ((rc = zxcrypt_volume_manager.UnsealWithDeviceKey(slot)) != ZX_OK) {
        printf("fshost: couldn't unseal zxcrypt manager device");
        return 0;
    }

    return 0;
}

disk_format_t ReformatDataPartition(fbl::unique_fd fd, zx_handle_t disk_channel,
                                    const char* device_path) {
    zx_status_t call_status;
    fbl::StringBuffer<PATH_MAX> path;
    path.Resize(path.capacity());
    size_t path_len;
    // Both the zxcrypt and minfs partitions have the same gpt guid, so here we
    // determine which one we actually need to format. We do this by looking up
    // the topological path, if it is the zxcrypt driver, then we format it as
    // minfs, otherwise as zxcrypt.
    if (fuchsia_device_ControllerGetTopologicalPath(disk_channel, &call_status, path.data(),
                                                    path.capacity(), &path_len) != ZX_OK) {
        return DISK_FORMAT_UNKNOWN;
    }
    const fbl::StringPiece kZxcryptPath("/zxcrypt/unsealed/block");
    if (fbl::StringPiece(path.begin() + path_len - kZxcryptPath.length()).compare(kZxcryptPath) == 0) {
        printf("fshost: Minfs data partition is corrupt. Will attempt to reformat %s\n", device_path);
        if (mkfs(device_path, DISK_FORMAT_MINFS, launch_stdio_sync, &default_mkfs_options) == ZX_OK) {
            return DISK_FORMAT_MINFS;
        }
    } else {
      printf("fshost: zxcrypt volume is corrupt. Will attempt to reformat %s\n", device_path);
      if (zxcrypt::FdioVolume::CreateWithDeviceKey(std::move(fd), nullptr) == ZX_OK) {
          return DISK_FORMAT_ZXCRYPT;
      }
    }
    return DISK_FORMAT_UNKNOWN;
}

// Attempts to reformat the partition at the device path. Returns the specific
// disk format if successful and unknown otherwise.  Currently only works for
// minfs and zxcrypt data partitions.
disk_format_t ReformatPartition(fbl::unique_fd fd, zx_handle_t disk_channel,
                                const char* device_path) {
    zx_status_t call_status, io_status;
    fuchsia_hardware_block_partition_GUID guid;
    io_status = fuchsia_hardware_block_partition_PartitionGetTypeGuid(disk_channel, &call_status,
                                                                      &guid);
    if (io_status != ZX_OK || call_status != ZX_OK) {
        return DISK_FORMAT_UNKNOWN;
    }
    if (gpt_is_data_guid(guid.value, GPT_GUID_LEN)) {
        return ReformatDataPartition(std::move(fd), disk_channel, device_path);
    }
    return DISK_FORMAT_UNKNOWN;
}

zx_status_t FormatMinfs(const fbl::unique_fd& block_device,
                        const fuchsia_hardware_block_BlockInfo& info) {

    fprintf(stderr, "fshost: Formatting minfs.\n");
    uint64_t device_size = info.block_size * info.block_count;
    fbl::unique_ptr<minfs::Bcache> bc;
    zx_status_t status;
    if ((status = minfs::Bcache::Create(&bc, block_device.duplicate(),
                                        static_cast<uint32_t>(device_size))) != ZX_OK) {
        fprintf(stderr, "fshost: Could not initialize minfs bcache.\n");
        return status;
    }
    minfs::MountOptions options = {};
    if ((status = Mkfs(options, std::move(bc))) != ZX_OK) {
        fprintf(stderr, "fshost: Could not format minfs filesystem.\n");
        return status;
    }
    printf("fshost: Minfs filesystem re-formatted. Expect data loss.\n");
    return ZX_OK;
}

zx_status_t BlockDeviceAdded(int dirfd, int event, const char* name, void* cookie) {
    auto watcher = static_cast<BlockWatcher*>(cookie);

    if (event != WATCH_EVENT_ADD_FILE) {
        return ZX_OK;
    }

    char device_path[PATH_MAX];
    sprintf(device_path, "%s/%s", PATH_DEV_BLOCK, name);

    fbl::unique_fd fd(openat(dirfd, name, O_RDWR));
    if (!fd) {
        return ZX_OK;
    }

    disk_format_t df = detect_disk_format(fd.get());
    fuchsia_hardware_block_BlockInfo info;
    fuchsia_hardware_block_partition_GUID guid;
    {
        fzl::UnownedFdioCaller disk_connection(fd.get());
        zx::unowned_channel disk(disk_connection.borrow_channel());

        zx_status_t io_status, call_status;
        io_status = fuchsia_hardware_block_BlockGetInfo(disk->get(), &call_status, &info);
        if (io_status != ZX_OK || call_status != ZX_OK) {
            return ZX_OK;
        }

        if (df == DISK_FORMAT_UNKNOWN && !watcher->Netbooting()) {
            df = ReformatPartition(fd.duplicate(), disk->get(), device_path);
        }

        if (info.flags & BLOCK_FLAG_BOOTPART) {
            fuchsia_device_ControllerBind(disk->get(), BOOTPART_DRIVER_LIB,
                                          STRLEN(BOOTPART_DRIVER_LIB), &call_status);
            return ZX_OK;
        }

        switch (df) {
        case DISK_FORMAT_GPT: {
            printf("fshost: %s: GPT?\n", device_path);
            // probe for partition table
            fuchsia_device_ControllerBind(disk->get(), GPT_DRIVER_LIB, STRLEN(GPT_DRIVER_LIB),
                                          &call_status);
            return ZX_OK;
        }
        case DISK_FORMAT_FVM: {
            printf("fshost: /dev/class/block/%s: FVM?\n", name);
            // probe for partition table
            fuchsia_device_ControllerBind(disk->get(), FVM_DRIVER_LIB, STRLEN(FVM_DRIVER_LIB),
                                          &call_status);
            return ZX_OK;
        }
        case DISK_FORMAT_MBR: {
            printf("fshost: %s: MBR?\n", device_path);
            // probe for partition table
            fuchsia_device_ControllerBind(disk->get(), MBR_DRIVER_LIB, STRLEN(MBR_DRIVER_LIB),
                                          &call_status);
            return ZX_OK;
        }
        case DISK_FORMAT_ZXCRYPT: {
            if (!watcher->Netbooting()) {
                printf("fshost: %s: zxcrypt?\n", device_path);
                // Bind and unseal the driver from a separate thread, since we
                // have to wait for a number of devices to do I/O and settle,
                // and we don't want to block block-watcher for any nontrivial
                // length of time.

                // We transfer fd to the spawned thread.  Since it's UB to cast
                // ints to pointers and back, we allocate the fd on the heap.
                int loose_fd = fd.release();
                int* raw_fd_ptr = new int(loose_fd);
                thrd_t th;
                int err = thrd_create_with_name(&th, &UnsealZxcrypt, raw_fd_ptr, "zxcrypt-unseal");
                if (err != thrd_success) {
                    printf("fshost: failed to spawn zxcrypt unseal thread");
                    close(loose_fd);
                    delete raw_fd_ptr;
                } else {
                    thrd_detach(th);
                }
            }
            return ZX_OK;
        }
        default:
            break;
        }

        io_status = fuchsia_hardware_block_partition_PartitionGetTypeGuid(disk->get(), &call_status,
                                                                          &guid);
        if (io_status != ZX_OK || call_status != ZX_OK) {
            return ZX_OK;
        }
    }

    // If we're in netbooting mode, then only bind drivers for partition
    // containers and the install partition, not regular filesystems.
    if (watcher->Netbooting()) {
        if (gpt_is_install_guid(guid.value, GPT_GUID_LEN)) {
            printf("fshost: mounting install partition\n");
            mount_options_t options = default_mount_options;
            MountMinfs(watcher, std::move(fd), &options);
            return ZX_OK;
        }

        return ZX_OK;
    }

    switch (df) {
    case DISK_FORMAT_BLOBFS: {
        const uint8_t expected_guid[GPT_GUID_LEN] = GUID_BLOB_VALUE;

        if (memcmp(guid.value, expected_guid, GPT_GUID_LEN)) {
            return ZX_OK;
        }
        fsck_options_t fsck_options = default_fsck_options;
        fsck_options.apply_journal = true;
        if (watcher->CheckFilesystem(device_path, DISK_FORMAT_BLOBFS, &fsck_options) != ZX_OK) {
            return ZX_OK;
        }

        mount_options_t options = default_mount_options;
        options.enable_journal = true;
        options.collect_metrics = true;
        zx_status_t status = watcher->MountBlob(std::move(fd), &options);
        if (status != ZX_OK) {
            printf("fshost: Failed to mount blobfs partition %s at %s: %s.\n", device_path,
                   PATH_BLOB, zx_status_get_string(status));
        } else {
            LaunchBlobInit(watcher);
        }
        return ZX_OK;
    }
    case DISK_FORMAT_MINFS: {
        printf("fshost: mounting minfs\n");
        fsck_options_t fsck_options = default_fsck_options;
        if (watcher->CheckFilesystem(device_path, DISK_FORMAT_MINFS, &fsck_options) != ZX_OK) {
            if (FormatMinfs(fd, info) != ZX_OK) {
                return ZX_OK;
            }
        }
        mount_options_t options = default_mount_options;
        MountMinfs(watcher, std::move(fd), &options);
        return ZX_OK;
    }
    case DISK_FORMAT_FAT: {
        // Use the GUID to avoid auto-mounting the EFI partition
        bool efi = gpt_is_efi_guid(guid.value, GPT_GUID_LEN);
        if (efi) {
            printf("fshost: not automounting efi\n");
            return ZX_OK;
        }
        mount_options_t options = default_mount_options;
        options.create_mountpoint = true;
        static int fat_counter = 0;
        char mountpath[FDIO_MAX_FILENAME + 64];
        snprintf(mountpath, sizeof(mountpath), "%s/fat-%d", "/fs" PATH_VOLUME, fat_counter++);
        options.wait_until_ready = false;
        printf("fshost: mounting fatfs\n");
        mount(fd.release(), mountpath, df, &options, LaunchFAT);
        return ZX_OK;
    }
    default:
        return ZX_OK;
    }
}

} // namespace

void BlockDeviceWatcher(fbl::unique_ptr<FsManager> fshost, bool netboot) {
    BlockWatcher watcher(std::move(fshost), netboot);

    fbl::unique_fd dirfd(open("/dev/class/block", O_DIRECTORY | O_RDONLY));
    if (dirfd) {
        fdio_watch_directory(dirfd.get(), BlockDeviceAdded, ZX_TIME_INFINITE, &watcher);
    }
}

} // namespace devmgr
