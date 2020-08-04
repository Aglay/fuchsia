// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <errno.h>
#include <lib/cksum.h>
#include <lib/zbitl/fd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <memory>
#include <new>

namespace zbitl {
namespace {

constexpr size_t kBufferSize = 8192;

using error_type = StorageTraits<fbl::unique_fd>::error_type;

}  // namespace

fitx::result<error_type, uint32_t> StorageTraits<fbl::unique_fd>::Capacity(
    const fbl::unique_fd& fd) {
  struct stat st;
  if (fstat(fd.get(), &st) < 0) {
    return fitx::error{errno};
  }
  auto size = std::min(static_cast<off_t>(std::numeric_limits<uint32_t>::max()), st.st_size);
  return fitx::ok(static_cast<uint32_t>(size));
}

fitx::result<error_type, zbi_header_t> StorageTraits<fbl::unique_fd>::Header(
    const fbl::unique_fd& fd, uint32_t offset) {
  zbi_header_t header;
  ssize_t n = pread(fd.get(), &header, sizeof(header), offset);
  if (n < 0) {
    return fitx::error{errno};
  }
  if (static_cast<size_t>(n) < sizeof(header)) {
    return fitx::error{ESPIPE};
  }
  return fitx::ok(header);
}

fitx::result<error_type> StorageTraits<fbl::unique_fd>::DoRead(const fbl::unique_fd& fd,
                                                               off_t offset, uint32_t length,
                                                               bool (*cb)(void*, ByteView),
                                                               void* arg) {
  if (length == 0) {
    cb(arg, {});
    return fitx::ok();
  }

  // This always copies, when mmap'ing might be better for large sizes.  But
  // address space is cheap, so users concerned with large sizes can just mmap
  // the whole ZBI in and use View<std::span> instead.
  auto size = [&]() { return std::min(static_cast<size_t>(length), kBufferSize); };
  std::unique_ptr<std::byte[]> buf{new std::byte[size()]};

  while (length > 0) {
    ssize_t n = pread(fd.get(), buf.get(), size(), offset);
    if (n < 0) {
      return fitx::error{errno};
    }
    if (n == 0) {
      return fitx::error{ESPIPE};
    }
    ZX_ASSERT(static_cast<size_t>(n) <= kBufferSize);
    if (!cb(arg, {buf.get(), static_cast<size_t>(n)})) {
      break;
    }
    offset += n;
    length -= static_cast<uint32_t>(n);
  }

  return fitx::ok();
}

fitx::result<error_type> StorageTraits<fbl::unique_fd>::Write(const fbl::unique_fd& fd,
                                                              uint32_t offset, ByteView data) {
  while (!data.empty()) {
    ssize_t n = pwrite(fd.get(), data.data(), data.size(), offset);
    if (n < 0) {
      return fitx::error{errno};
    }
    ZX_ASSERT(static_cast<size_t>(n) <= data.size());
    offset += static_cast<uint32_t>(n);
    data.remove_prefix(static_cast<size_t>(n));
  }
  return fitx::ok();
}

}  // namespace zbitl
