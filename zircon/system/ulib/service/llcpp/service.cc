// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/io/llcpp/fidl.h>
#include <lib/fidl/llcpp/array.h>
#include <lib/fidl/llcpp/connect_service.h>
#include <lib/fit/result.h>
#include <lib/service/llcpp/service.h>
#include <zircon/device/vfs.h>

namespace llcpp::sys {

namespace {

constexpr uint64_t kMaxFilename = ::llcpp::fuchsia::io::MAX_FILENAME;

// Max path length will be two path components, separated by a file separator.
constexpr uint64_t kMaxPath = (2 * kMaxFilename) + 1;

::zx::status<::fidl::StringView> ValidateAndJoinPath(::fidl::Array<char, kMaxPath>* buffer,
                                                     ::fidl::StringView service,
                                                     ::fidl::StringView instance) {
  if (service.empty() || service.size() > kMaxFilename) {
    return ::zx::error(ZX_ERR_INVALID_ARGS);
  }
  if (instance.size() > kMaxFilename) {
    return ::zx::error(ZX_ERR_INVALID_ARGS);
  }
  if (service[0] == '/') {
    return ::zx::error(ZX_ERR_INVALID_ARGS);
  }

  const uint64_t path_size = service.size() + instance.size() + 1;
  ZX_ASSERT(path_size <= kMaxPath);

  char* path_cursor = buffer->data();
  memcpy(path_cursor, service.data(), service.size());
  path_cursor += service.size();
  *path_cursor++ = '/';
  memcpy(path_cursor, instance.data(), instance.size());
  return ::zx::ok(::fidl::unowned_str(buffer->data(), path_size));
}

}  // namespace

namespace internal {

::zx::status<> DirectoryOpenFunc(::zx::unowned_channel dir, ::fidl::StringView path,
                                 ::zx::channel remote) {
  constexpr uint32_t flags =
      ::llcpp::fuchsia::io::OPEN_RIGHT_READABLE | ::llcpp::fuchsia::io::OPEN_RIGHT_WRITABLE;
  ::llcpp::fuchsia::io::Directory::ResultOf::Open result =
      ::llcpp::fuchsia::io::Directory::Call::Open(dir, flags, uint32_t(0755), std::move(path),
                                                  std::move(remote));
  return ::zx::make_status(result.status());
}

}  // namespace internal

::zx::status<> OpenNamedServiceAt(::zx::unowned_channel dir, cpp17::string_view service,
                                  cpp17::string_view instance, ::zx::channel remote) {
  ::fidl::Array<char, kMaxPath> path_buffer;
  ::zx::status<::fidl::StringView> path_result = ValidateAndJoinPath(
      &path_buffer, ::fidl::unowned_str(service), ::fidl::unowned_str(instance));
  if (!path_result.is_ok()) {
    return path_result.take_error();
  }
  return internal::DirectoryOpenFunc(std::move(dir), std::move(path_result.value()),
                                     std::move(remote));
}

}  // namespace llcpp::sys
