// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/ledger/storage/testing/page_storage_empty_impl.h"

#include "lib/fxl/logging.h"

namespace storage {

PageId PageStorageEmptyImpl::GetId() {
  FXL_NOTIMPLEMENTED();
  return "NOT_IMPLEMENTED";
}

void PageStorageEmptyImpl::SetSyncDelegate(PageSyncDelegate* /*page_sync*/) {
  FXL_NOTIMPLEMENTED();
}

void PageStorageEmptyImpl::GetHeadCommitIds(
    std::function<void(Status, std::vector<CommitId>)> callback) {
  FXL_NOTIMPLEMENTED();
  callback(Status::NOT_IMPLEMENTED, std::vector<CommitId>());
}

void PageStorageEmptyImpl::GetCommit(
    CommitIdView /*commit_id*/,
    std::function<void(Status, std::unique_ptr<const Commit>)> callback) {
  FXL_NOTIMPLEMENTED();
  callback(Status::NOT_IMPLEMENTED, nullptr);
}

void PageStorageEmptyImpl::AddCommitsFromSync(
    std::vector<CommitIdAndBytes> /*ids_and_bytes*/, ChangeSource /*source*/,
    std::function<void(Status)> callback) {
  FXL_NOTIMPLEMENTED();
  callback(Status::NOT_IMPLEMENTED);
}

void PageStorageEmptyImpl::StartCommit(
    const CommitId& /*commit_id*/, JournalType /*journal_type*/,
    std::function<void(Status, std::unique_ptr<Journal>)> callback) {
  FXL_NOTIMPLEMENTED();
  callback(Status::NOT_IMPLEMENTED, nullptr);
}

void PageStorageEmptyImpl::StartMergeCommit(
    const CommitId& /*left*/, const CommitId& /*right*/,
    std::function<void(Status, std::unique_ptr<Journal>)> callback) {
  FXL_NOTIMPLEMENTED();
  callback(Status::NOT_IMPLEMENTED, nullptr);
}

void PageStorageEmptyImpl::CommitJournal(
    std::unique_ptr<Journal> /*journal*/,
    std::function<void(Status, std::unique_ptr<const storage::Commit>)>
        callback) {
  FXL_NOTIMPLEMENTED();
  callback(Status::NOT_IMPLEMENTED, nullptr);
}

void PageStorageEmptyImpl::RollbackJournal(
    std::unique_ptr<Journal> /*journal*/,
    std::function<void(Status)> callback) {
  FXL_NOTIMPLEMENTED();
  callback(Status::NOT_IMPLEMENTED);
}

Status PageStorageEmptyImpl::AddCommitWatcher(CommitWatcher* /*watcher*/) {
  FXL_NOTIMPLEMENTED();
  return Status::NOT_IMPLEMENTED;
}

Status PageStorageEmptyImpl::RemoveCommitWatcher(CommitWatcher* /*watcher*/) {
  FXL_NOTIMPLEMENTED();
  return Status::NOT_IMPLEMENTED;
}

void PageStorageEmptyImpl::IsSynced(
    std::function<void(Status, bool)> callback) {
  FXL_NOTIMPLEMENTED();
  callback(Status::NOT_IMPLEMENTED, false);
}

void PageStorageEmptyImpl::GetUnsyncedCommits(
    std::function<void(Status, std::vector<std::unique_ptr<const Commit>>)>
        callback) {
  FXL_NOTIMPLEMENTED();
  callback(Status::NOT_IMPLEMENTED, {});
}

void PageStorageEmptyImpl::MarkCommitSynced(
    const CommitId& /*commit_id*/, std::function<void(Status)> callback) {
  FXL_NOTIMPLEMENTED();
  callback(Status::NOT_IMPLEMENTED);
}

void PageStorageEmptyImpl::GetUnsyncedPieces(
    std::function<void(Status, std::vector<ObjectIdentifier>)> callback) {
  FXL_NOTIMPLEMENTED();
  callback(Status::NOT_IMPLEMENTED, std::vector<ObjectIdentifier>());
}

void PageStorageEmptyImpl::MarkPieceSynced(
    ObjectIdentifier /*object_identifier*/,
    std::function<void(Status)> callback) {
  FXL_NOTIMPLEMENTED();
  callback(Status::NOT_IMPLEMENTED);
}

void PageStorageEmptyImpl::IsPieceSynced(
    ObjectIdentifier object_identifier,
    std::function<void(Status, bool)> callback) {
  FXL_NOTIMPLEMENTED();
  callback(Status::NOT_IMPLEMENTED, false);
}

void PageStorageEmptyImpl::AddObjectFromLocal(
    std::unique_ptr<DataSource> /*data_source*/,
    std::function<void(Status, ObjectIdentifier)> callback) {
  FXL_NOTIMPLEMENTED();
  callback(Status::NOT_IMPLEMENTED, {});
}

void PageStorageEmptyImpl::GetObject(
    ObjectIdentifier /*object_identifier*/, Location /*location*/,
    std::function<void(Status, std::unique_ptr<const Object>)> callback) {
  FXL_NOTIMPLEMENTED();
  callback(Status::NOT_IMPLEMENTED, nullptr);
}

void PageStorageEmptyImpl::GetPiece(
    ObjectIdentifier /*object_identifier*/,
    std::function<void(Status, std::unique_ptr<const Object>)> callback) {
  callback(Status::NOT_IMPLEMENTED, nullptr);
}

void PageStorageEmptyImpl::SetSyncMetadata(
    fxl::StringView /*key*/, fxl::StringView /*value*/,
    std::function<void(Status)> callback) {
  FXL_NOTIMPLEMENTED();
  callback(Status::NOT_IMPLEMENTED);
}

void PageStorageEmptyImpl::GetSyncMetadata(
    fxl::StringView /*key*/,
    std::function<void(Status, std::string)> callback) {
  FXL_NOTIMPLEMENTED();
  callback(Status::NOT_IMPLEMENTED, "");
}

void PageStorageEmptyImpl::GetCommitContents(
    const Commit& /*commit*/, std::string /*min_key*/,
    std::function<bool(Entry)> /*on_next*/,
    std::function<void(Status)> on_done) {
  FXL_NOTIMPLEMENTED();
  on_done(Status::NOT_IMPLEMENTED);
}

void PageStorageEmptyImpl::GetEntryFromCommit(
    const Commit& /*commit*/, std::string /*key*/,
    std::function<void(Status, Entry)> callback) {
  FXL_NOTIMPLEMENTED();
  callback(Status::NOT_IMPLEMENTED, Entry());
}

void PageStorageEmptyImpl::GetCommitContentsDiff(
    const Commit& /*base_commit*/, const Commit& /*other_commit*/,
    std::string /*min_key*/, std::function<bool(EntryChange)> /*on_next_diff*/,
    std::function<void(Status)> on_done) {
  FXL_NOTIMPLEMENTED();
  on_done(Status::NOT_IMPLEMENTED);
}

void PageStorageEmptyImpl::GetThreeWayContentsDiff(
    const Commit& /*base_commit*/, const Commit& /*left_commit*/,
    const Commit& /*right_commit*/, std::string /*min_key*/,
    std::function<bool(ThreeWayChange)> /*on_next_diff*/,
    std::function<void(Status)> on_done) {
  FXL_NOTIMPLEMENTED();
  on_done(Status::NOT_IMPLEMENTED);
}

}  // namespace storage
