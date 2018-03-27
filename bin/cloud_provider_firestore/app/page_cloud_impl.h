// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_CLOUD_PROVIDER_FIRESTORE_APP_PAGE_CLOUD_IMPL_H_
#define PERIDOT_BIN_CLOUD_PROVIDER_FIRESTORE_APP_PAGE_CLOUD_IMPL_H_

#include <memory>
#include <utility>

#include "lib/cloud_provider/fidl/cloud_provider.fidl.h"
#include "lib/fidl/cpp/array.h"
#include "lib/fidl/cpp/bindings/binding.h"
#include "lib/fxl/functional/closure.h"
#include "lib/fxl/macros.h"
#include "peridot/bin/cloud_provider_firestore/app/credentials_provider.h"
#include "peridot/bin/cloud_provider_firestore/firestore/firestore_service.h"

namespace cloud_provider_firestore {

class PageCloudImpl : public cloud_provider::PageCloud {
 public:
  explicit PageCloudImpl(
      std::string page_path,
      CredentialsProvider* credentials_provider,
      FirestoreService* firestore_service,
      f1dl::InterfaceRequest<cloud_provider::PageCloud> request);
  ~PageCloudImpl() override;

  void set_on_empty(const fxl::Closure& on_empty) { on_empty_ = on_empty; }

 private:
  // cloud_provider::PageCloud:
  void AddCommits(f1dl::VectorPtr<cloud_provider::CommitPtr> commits,
                  const AddCommitsCallback& callback) override;
  void GetCommits(f1dl::VectorPtr<uint8_t> min_position_token,
                  const GetCommitsCallback& callback) override;
  void AddObject(f1dl::VectorPtr<uint8_t> id,
                 fsl::SizedVmoTransportPtr data,
                 const AddObjectCallback& callback) override;
  void GetObject(f1dl::VectorPtr<uint8_t> id,
                 const GetObjectCallback& callback) override;
  void SetWatcher(
      f1dl::VectorPtr<uint8_t> min_position_token,
      f1dl::InterfaceHandle<cloud_provider::PageCloudWatcher> watcher,
      const SetWatcherCallback& callback) override;

  const std::string page_path_;
  CredentialsProvider* const credentials_provider_;
  FirestoreService* const firestore_service_;

  f1dl::Binding<cloud_provider::PageCloud> binding_;
  fxl::Closure on_empty_;

  FXL_DISALLOW_COPY_AND_ASSIGN(PageCloudImpl);
};

}  // namespace cloud_provider_firestore

#endif  // PERIDOT_BIN_CLOUD_PROVIDER_FIRESTORE_APP_PAGE_CLOUD_IMPL_H_
