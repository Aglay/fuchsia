// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/cloud_provider_firestore/app/page_cloud_impl.h"

#include "lib/fsl/vmo/sized_vmo.h"
#include "lib/fxl/functional/make_copyable.h"

namespace cloud_provider_firestore {

PageCloudImpl::PageCloudImpl(
    f1dl::InterfaceRequest<cloud_provider::PageCloud> request)
    : binding_(this, std::move(request)) {
  // The class shuts down when the client connection is disconnected.
  binding_.set_error_handler([this] {
    if (on_empty_) {
      on_empty_();
    }
  });
}

PageCloudImpl::~PageCloudImpl() {}

void PageCloudImpl::AddCommits(
    f1dl::VectorPtr<cloud_provider::CommitPtr> /*commits*/,
    const AddCommitsCallback& callback) {
  FXL_NOTIMPLEMENTED();
  callback(cloud_provider::Status::INTERNAL_ERROR);
}

void PageCloudImpl::GetCommits(f1dl::VectorPtr<uint8_t> /*min_position_token*/,
                               const GetCommitsCallback& callback) {
  FXL_NOTIMPLEMENTED();
  callback(cloud_provider::Status::INTERNAL_ERROR, nullptr, nullptr);
}

void PageCloudImpl::AddObject(f1dl::VectorPtr<uint8_t> /*id*/,
                              fsl::SizedVmoTransportPtr /*data*/,
                              const AddObjectCallback& callback) {
  FXL_NOTIMPLEMENTED();
  callback(cloud_provider::Status::INTERNAL_ERROR);
}

void PageCloudImpl::GetObject(f1dl::VectorPtr<uint8_t> /*id*/,
                              const GetObjectCallback& callback) {
  FXL_NOTIMPLEMENTED();
  callback(cloud_provider::Status::INTERNAL_ERROR, 0u, zx::socket());
}

void PageCloudImpl::SetWatcher(
    f1dl::VectorPtr<uint8_t> /*min_position_token*/,
    f1dl::InterfaceHandle<cloud_provider::PageCloudWatcher> /*watcher*/,
    const SetWatcherCallback& callback) {
  FXL_NOTIMPLEMENTED();
  callback(cloud_provider::Status::INTERNAL_ERROR);
}

}  // namespace cloud_provider_firestore
