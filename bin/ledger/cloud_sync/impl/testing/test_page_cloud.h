// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_LEDGER_CLOUD_SYNC_IMPL_TESTING_TEST_PAGE_CLOUD_H_
#define PERIDOT_BIN_LEDGER_CLOUD_SYNC_IMPL_TESTING_TEST_PAGE_CLOUD_H_

#include "lib/cloud_provider/fidl/cloud_provider.fidl.h"
#include "lib/fidl/cpp/array.h"
#include "lib/fidl/cpp/bindings/binding.h"
#include "lib/fxl/macros.h"
#include "peridot/bin/ledger/encryption/fake/fake_encryption_service.h"

namespace cloud_sync {

struct ReceivedCommit {
  std::string id;
  std::string data;
};

cloud_provider::CommitPtr MakeTestCommit(
    encryption::FakeEncryptionService* encryption_service,
    const std::string& id,
    const std::string& data);

class TestPageCloud : public cloud_provider::PageCloud {
 public:
  explicit TestPageCloud(
      f1dl::InterfaceRequest<cloud_provider::PageCloud> request);
  ~TestPageCloud() override;

  void RunPendingCallbacks();

  cloud_provider::Status status_to_return = cloud_provider::Status::OK;
  cloud_provider::Status commit_status_to_return = cloud_provider::Status::OK;
  cloud_provider::Status object_status_to_return = cloud_provider::Status::OK;

  // AddCommits().
  unsigned int add_commits_calls = 0u;
  std::vector<ReceivedCommit> received_commits;

  // GetCommits().
  unsigned int get_commits_calls = 0u;
  f1dl::VectorPtr<cloud_provider::CommitPtr> commits_to_return;
  f1dl::VectorPtr<uint8_t> position_token_to_return;

  // AddObject().
  unsigned int add_object_calls = 0u;
  std::map<std::string, std::string> received_objects;
  bool delay_add_object_callbacks = false;
  std::vector<fxl::Closure> pending_add_object_callbacks;
  bool reset_object_status_after_call = false;

  // GetObject().
  unsigned int get_object_calls = 0u;
  std::map<std::string, std::string> objects_to_return;

  // SetWatcher().
  std::vector<std::string> set_watcher_position_tokens;
  cloud_provider::PageCloudWatcherPtr set_watcher;

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

  f1dl::Binding<cloud_provider::PageCloud> binding_;

  FXL_DISALLOW_COPY_AND_ASSIGN(TestPageCloud);
};

}  // namespace cloud_sync

#endif  // PERIDOT_BIN_LEDGER_CLOUD_SYNC_IMPL_TESTING_TEST_PAGE_CLOUD_H_
