// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/fdio/io.h>
#include <zircon/syscalls.h>
#include <zircon/types.h>

#include "private.h"

namespace fuchsia = ::llcpp::fuchsia;

zx_status_t fdio_default_get_token(fdio_t* io, zx_handle_t* out) { return ZX_ERR_NOT_SUPPORTED; }

zx_status_t fdio_default_get_attr(fdio_t* io, fuchsia::io::NodeAttributes* out) {
  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t fdio_default_set_attr(fdio_t* io, uint32_t flags,
                                  const fuchsia::io::NodeAttributes* attr) {
  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t fdio_default_readdir(fdio_t* io, void* ptr, size_t max, size_t* actual) {
  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t fdio_default_rewind(fdio_t* io) { return ZX_ERR_NOT_SUPPORTED; }

zx_status_t fdio_default_unlink(fdio_t* io, const char* path, size_t len) {
  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t fdio_default_truncate(fdio_t* io, off_t off) { return ZX_ERR_NOT_SUPPORTED; }

zx_status_t fdio_default_rename(fdio_t* io, const char* src, size_t srclen, zx_handle_t dst_token,
                                const char* dst, size_t dstlen) {
  zx_handle_close(dst_token);
  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t fdio_default_link(fdio_t* io, const char* src, size_t srclen, zx_handle_t dst_token,
                              const char* dst, size_t dstlen) {
  zx_handle_close(dst_token);
  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t fdio_default_get_flags(fdio_t* io, uint32_t* out_flags) { return ZX_ERR_NOT_SUPPORTED; }

zx_status_t fdio_default_set_flags(fdio_t* io, uint32_t flags) { return ZX_ERR_NOT_SUPPORTED; }

zx_status_t fdio_default_open(fdio_t* io, const char* path, uint32_t flags, uint32_t mode,
                              fdio_t** out) {
  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t fdio_default_clone(fdio_t* io, zx_handle_t* out_handle) { return ZX_ERR_NOT_SUPPORTED; }

zx_status_t fdio_default_unwrap(fdio_t* io, zx_handle_t* out_handle) {
  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t fdio_default_shutdown(fdio_t* io, int how) { return ZX_ERR_WRONG_TYPE; }

ssize_t fdio_default_ioctl(fdio_t* io, uint32_t op, const void* in_buf, size_t in_len,
                           void* out_buf, size_t out_len) {
  return ZX_ERR_NOT_SUPPORTED;
}

void fdio_default_wait_begin(fdio_t* io, uint32_t events, zx_handle_t* handle,
                             zx_signals_t* _signals) {
  *handle = ZX_HANDLE_INVALID;
}

void fdio_default_wait_end(fdio_t* io, zx_signals_t signals, uint32_t* _events) {}

zx_status_t fdio_default_posix_ioctl(fdio_t* io, int req, va_list va) {
  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t fdio_default_get_vmo(fdio_t* io, int flags, zx::vmo* out) {
  return ZX_ERR_NOT_SUPPORTED;
}
