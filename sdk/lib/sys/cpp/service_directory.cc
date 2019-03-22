// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/sys/cpp/service_directory.h>

#include <lib/fdio/directory.h>
#include <lib/zx/channel.h>

namespace sys {
namespace {

zx::channel OpenServiceRoot() {
  zx::channel request, service_root;
  if (zx::channel::create(0, &request, &service_root) != ZX_OK)
    return zx::channel();
  if (fdio_service_connect("/svc/.", request.release()) != ZX_OK)
    return zx::channel();
  return service_root;
}

}  // namespace

ServiceDirectory::ServiceDirectory() = default;

ServiceDirectory::ServiceDirectory(zx::channel directory)
    : directory_(std::move(directory)) {}

ServiceDirectory::ServiceDirectory(
    fidl::InterfaceHandle<fuchsia::io::Directory> directory)
    : ServiceDirectory(directory.TakeChannel()) {}

ServiceDirectory::~ServiceDirectory() = default;

std::shared_ptr<ServiceDirectory> ServiceDirectory::CreateFromNamespace() {
  return std::make_shared<ServiceDirectory>(OpenServiceRoot());
}

std::shared_ptr<ServiceDirectory> ServiceDirectory::CreateWithRequest(
    zx::channel* out_request) {
  zx::channel directory;
  zx_status_t status = zx::channel::create(0, &directory, out_request);
  if (status != ZX_OK) {
    return std::make_shared<ServiceDirectory>(ServiceDirectory());
  }
  return std::make_shared<ServiceDirectory>(
      ServiceDirectory(std::move(directory)));
}

std::shared_ptr<ServiceDirectory> ServiceDirectory::CreateWithRequest(
    fidl::InterfaceRequest<fuchsia::io::Directory>* out_request) {
  zx::channel request;
  auto directory = CreateWithRequest(&request);
  out_request->set_channel(std::move(request));
  return directory;
}

std::shared_ptr<ServiceDirectory> ServiceDirectory::CreateWithRequest2(
    zx::channel* out_request) {
  return CreateWithRequest(out_request);
}

std::shared_ptr<ServiceDirectory> ServiceDirectory::CreateWithRequest2(
    fidl::InterfaceRequest<fuchsia::io::Directory>* out_request) {
  return CreateWithRequest(out_request);
}

zx_status_t ServiceDirectory::Connect(const std::string& interface_name,
                                      zx::channel channel) const {
  return fdio_service_connect_at(directory_.get(), interface_name.c_str(),
                                 channel.release());
}

fidl::InterfaceHandle<fuchsia::io::Directory> ServiceDirectory::CloneChannel()
    const {
  fidl::InterfaceHandle<fuchsia::io::Directory> dir;
  CloneChannel(dir.NewRequest());
  return dir;
}

zx_status_t ServiceDirectory::CloneChannel(
    fidl::InterfaceRequest<fuchsia::io::Directory> dir) const {
  return fdio_service_clone_to(directory_.get(), dir.TakeChannel().release());
}

}  // namespace sys
