// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <unistd.h>

#include <fbl/auto_call.h>
#include <fbl/string_buffer.h>
#include <fuchsia/device/c/fidl.h>
#include <fuchsia/hardware/block/c/fidl.h>
#include <fuchsia/hardware/block/volume/c/fidl.h>
#include <lib/fdio/directory.h>
#include <lib/fzl/fdio.h>
#include <lib/zircon-internal/debug.h>
#include <ramdevice-client/ramdisk.h> // Why does wait_for_device() come from here?
#include <zircon/status.h>
#include <zxcrypt/fdio-volume.h>
#include <zxcrypt/volume.h>

#include <utility>

#define ZXDEBUG 0

namespace zxcrypt {

// The zxcrypt driver
const char* kDriverLib = "/boot/driver/zxcrypt.so";

FdioVolume::FdioVolume(fbl::unique_fd&& fd) : Volume(), fd_(std::move(fd)) {}

zx_status_t FdioVolume::Init(fbl::unique_fd fd, fbl::unique_ptr<FdioVolume>* out) {
    zx_status_t rc;

    if (!fd || !out) {
        xprintf("bad parameter(s): fd=%d, out=%p\n", fd.get(), out);
        return ZX_ERR_INVALID_ARGS;
    }

    fbl::AllocChecker ac;
    fbl::unique_ptr<FdioVolume> volume(new (&ac) FdioVolume(std::move(fd)));
    if (!ac.check()) {
        xprintf("allocation failed: %zu bytes\n", sizeof(FdioVolume));
        return ZX_ERR_NO_MEMORY;
    }

    if ((rc = volume->Init()) != ZX_OK) {
        return rc;
    }

    *out = std::move(volume);
    return ZX_OK;
}

zx_status_t FdioVolume::Create(fbl::unique_fd fd, const crypto::Secret& key,
                                   fbl::unique_ptr<FdioVolume>* out) {
    zx_status_t rc;

    fbl::unique_ptr<FdioVolume> volume;
    if ((rc = FdioVolume::Init(std::move(fd), &volume)) != ZX_OK ||
        (rc = volume->CreateBlock()) != ZX_OK || (rc = volume->SealBlock(key, 0)) != ZX_OK ||
        (rc = volume->CommitBlock()) != ZX_OK) {
        return rc;
    }

    if (out) {
        *out = std::move(volume);
    }
    return ZX_OK;
}

zx_status_t FdioVolume::Unlock(fbl::unique_fd fd, const crypto::Secret& key, key_slot_t slot,
                                   fbl::unique_ptr<FdioVolume>* out) {
    zx_status_t rc;

    fbl::unique_ptr<FdioVolume> volume;
    if ((rc = FdioVolume::Init(std::move(fd), &volume)) != ZX_OK ||
        (rc = volume->Unlock(key, slot)) != ZX_OK) {
        return rc;
    }

    *out = std::move(volume);
    return ZX_OK;
}

zx_status_t FdioVolume::Unlock(const crypto::Secret& key, key_slot_t slot) {
    return Volume::Unlock(key, slot);
}

// Configuration methods
zx_status_t FdioVolume::Enroll(const crypto::Secret& key, key_slot_t slot) {
    zx_status_t rc;

    if ((rc = SealBlock(key, slot)) != ZX_OK || (rc = CommitBlock()) != ZX_OK) {
        return rc;
    }

    return ZX_OK;
}

zx_status_t FdioVolume::Revoke(key_slot_t slot) {
    zx_status_t rc;

    zx_off_t off;
    crypto::Bytes invalid;
    if ((rc = GetSlotOffset(slot, &off)) != ZX_OK || (rc = invalid.Randomize(slot_len_)) != ZX_OK ||
        (rc = block_.Copy(invalid, off)) != ZX_OK || (rc = CommitBlock()) != ZX_OK) {
        return rc;
    }

    return ZX_OK;
}

zx_status_t FdioVolume::Init() {
    return Volume::Init();
}

zx_status_t FdioVolume::Open(const zx::duration& timeout, fbl::unique_fd* out) {
    zx_status_t rc;

    fzl::UnownedFdioCaller caller(fd_.get());
    if (!caller) {
        xprintf("could not convert fd to io\n");
        return ZX_ERR_BAD_STATE;
    }

    // Get the full device path
    fbl::StringBuffer<PATH_MAX> path;
    path.Resize(path.capacity());
    zx_status_t call_status;
    size_t path_len;
    rc = fuchsia_device_ControllerGetTopologicalPath(caller.borrow_channel(),
                                                     &call_status, path.data(),
                                                     path.capacity(), &path_len);
    if (rc == ZX_OK) {
        rc = call_status;
    }
    if (rc != ZX_OK) {
        xprintf("could not find parent device: %s\n", zx_status_get_string(rc));
        return rc;
    }
    path.Resize(path_len);
    path.Append("/zxcrypt/unsealed/block");

    // Early return if already bound
    fbl::unique_fd fd(open(path.c_str(), O_RDWR));
    if (fd) {
        out->reset(fd.release());
        return ZX_OK;
    }

    // Bind the device
    rc = fuchsia_device_ControllerBind(caller.borrow_channel(), kDriverLib,
                                       strlen(kDriverLib), &call_status);
    if (rc == ZX_OK) {
        rc = call_status;
    }
    if (rc != ZX_OK) {
        xprintf("could not bind zxcrypt driver: %s\n", zx_status_get_string(rc));
        return rc;
    }
    if ((rc = wait_for_device(path.c_str(), timeout.get())) != ZX_OK) {
        xprintf("zxcrypt driver failed to bind: %s\n", zx_status_get_string(rc));
        return rc;
    }
    fd.reset(open(path.c_str(), O_RDWR));
    if (!fd) {
        xprintf("failed to open zxcrypt volume\n");
        return ZX_ERR_NOT_FOUND;
    }

    out->reset(fd.release());
    return ZX_OK;
}

zx_status_t FdioVolume::GetBlockInfo(BlockInfo* out) {
    zx_status_t rc;
    zx_status_t call_status;
    fzl::UnownedFdioCaller caller(fd_.get());
    if (!caller) {
        return ZX_ERR_BAD_STATE;
    }
    fuchsia_hardware_block_BlockInfo block_info;
    if ((rc = fuchsia_hardware_block_BlockGetInfo(caller.borrow_channel(),
                                                  &call_status, &block_info)) != ZX_OK) {
        return rc;
    }
    if (call_status != ZX_OK) {
        return call_status;
    }

    out->block_count = block_info.block_count;
    out->block_size = block_info.block_size;
    return ZX_OK;
}

zx_status_t FdioVolume::GetFvmSliceSize(uint64_t* out) {
    zx_status_t rc;
    zx_status_t call_status;
    fzl::UnownedFdioCaller caller(fd_.get());
    if (!caller) {
        return ZX_ERR_BAD_STATE;
    }

    // When this function is called, we're not yet sure if the underlying device
    // actually implements the block protocol, and we use the return value here
    // to tell us if we should utilize FVM-specific codepaths or not.
    // If the underlying channel doesn't respond to volume methods, when we call
    // a method from fuchsia.hardware.block.volume the FIDL channel will be
    // closed and we'll be unable to do other calls to it.  So before making
    // this call, we clone the channel.
    zx::channel channel(fdio_service_clone(caller.borrow_channel()));

    fuchsia_hardware_block_volume_VolumeInfo volume_info;
    if ((rc = fuchsia_hardware_block_volume_VolumeQuery(channel.get(),
                                                        &call_status, &volume_info)) != ZX_OK) {
        if (rc == ZX_ERR_PEER_CLOSED) {
            // The channel being closed here means that the thing at the other
            // end of this channel does not speak the FVM protocol, and has
            // closed the channel on us.  Return the appropriate error to signal
            // that we shouldn't bother with any of the FVM codepaths.
            return ZX_ERR_NOT_SUPPORTED;
        }
        return rc;
    }
    if (call_status != ZX_OK) {
        return call_status;
    }

    *out = volume_info.slice_size;
    return ZX_OK;
}

zx_status_t FdioVolume::DoBlockFvmVsliceQuery(uint64_t vslice_start,
                                              SliceRegion ranges[Volume::MAX_SLICE_REGIONS],
                                              uint64_t* slice_count) {
    static_assert(fuchsia_hardware_block_volume_MAX_SLICE_REQUESTS == Volume::MAX_SLICE_REGIONS,
                  "block volume slice response count must match");
    zx_status_t rc;
    zx_status_t call_status;
    fzl::UnownedFdioCaller caller(fd_.get());
    if (!caller) {
        return ZX_ERR_BAD_STATE;
    }
    fuchsia_hardware_block_volume_VsliceRange tmp_ranges[Volume::MAX_SLICE_REGIONS];
    uint64_t range_count;

    if ((rc =
         fuchsia_hardware_block_volume_VolumeQuerySlices(caller.borrow_channel(),
                                                         &vslice_start, 1,
                                                         &call_status,
                                                         tmp_ranges,
                                                         &range_count)) != ZX_OK) {
        return rc;
    }
    if (call_status != ZX_OK) {
        return call_status;
    }

    if (range_count > Volume::MAX_SLICE_REGIONS) {
        // Should be impossible.  Trust nothing.
        return ZX_ERR_BAD_STATE;
    }

    *slice_count = range_count;
    for (size_t i = 0; i < range_count; i++) {
        ranges[i].allocated = tmp_ranges[i].allocated;
        ranges[i].count = tmp_ranges[i].count;
    }

    return ZX_OK;
}

zx_status_t FdioVolume::DoBlockFvmExtend(uint64_t start_slice, uint64_t slice_count) {
    zx_status_t rc;
    zx_status_t call_status;
    fzl::UnownedFdioCaller caller(fd_.get());
    if (!caller) {
        return ZX_ERR_BAD_STATE;
    }
    if ((rc = fuchsia_hardware_block_volume_VolumeExtend(caller.borrow_channel(),
                                                         start_slice,
                                                         slice_count,
                                                         &call_status)) != ZX_OK) {
        return rc;
    }
    if (call_status != ZX_OK) {
        return call_status;
    }

    return ZX_OK;
}

zx_status_t FdioVolume::Read() {
    if (lseek(fd_.get(), offset_, SEEK_SET) < 0) {
        xprintf("lseek(%d, %" PRIu64 ", SEEK_SET) failed: %s\n", fd_.get(), offset_,
                strerror(errno));
        return ZX_ERR_IO;
    }
    ssize_t res;
    if ((res = read(fd_.get(), block_.get(), block_.len())) < 0) {
        xprintf("read(%d, %p, %zu) failed: %s\n", fd_.get(), block_.get(), block_.len(),
                strerror(errno));
        return ZX_ERR_IO;
    }
    if (static_cast<size_t>(res) != block_.len()) {
        xprintf("short read: have %zd, need %zu\n", res, block_.len());
        return ZX_ERR_IO;
    }

    return ZX_OK;
}

zx_status_t FdioVolume::Write() {
    if (lseek(fd_.get(), offset_, SEEK_SET) < 0) {
        xprintf("lseek(%d, %" PRIu64 ", SEEK_SET) failed: %s\n", fd_.get(), offset_,
                strerror(errno));
        return ZX_ERR_IO;
    }
    ssize_t res;
    if ((res = write(fd_.get(), block_.get(), block_.len())) < 0) {
        xprintf("write(%d, %p, %zu) failed: %s\n", fd_.get(), block_.get(), block_.len(),
                strerror(errno));
        return ZX_ERR_IO;
    }
    if (static_cast<size_t>(res) != block_.len()) {
        xprintf("short write: have %zd, need %zu\n", res, block_.len());
        return ZX_ERR_IO;
    }
    return ZX_OK;
}

} // namespace zxcrypt
