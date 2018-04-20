// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/cloud_provider_firestore/app/page_cloud_impl.h"

#include "garnet/lib/callback/scoped_callback.h"
#include "lib/fsl/socket/strings.h"
#include "lib/fsl/vmo/sized_vmo.h"
#include "lib/fsl/vmo/strings.h"
#include "lib/fxl/functional/make_copyable.h"
#include "lib/fxl/random/uuid.h"
#include "lib/fxl/strings/concatenate.h"
#include "peridot/bin/cloud_provider_firestore/app/grpc_status.h"
#include "peridot/bin/cloud_provider_firestore/firestore/encoding.h"
#include "peridot/lib/convert/convert.h"

namespace cloud_provider_firestore {
namespace {

constexpr char kSeparator[] = "/";
constexpr char kObjectCollection[] = "objects";
constexpr char kCommitLogCollection[] = "commit-log";
constexpr char kDataKey[] = "data";
constexpr char kTimestampField[] = "timestamp";
constexpr size_t kFirestoreMaxDocumentSize = 1'000'000;
// Ledger stores objects chunked to ~64k, so even 500kB is more than should ever
// be needed.
constexpr size_t kMaxObjectSize = kFirestoreMaxDocumentSize / 2;

std::string GetObjectPath(fxl::StringView page_path,
                          fxl::StringView object_id) {
  std::string encoded_object_id = EncodeKey(object_id);
  return fxl::Concatenate({page_path, kSeparator, kObjectCollection, kSeparator,
                           encoded_object_id});
}

std::string GetCommitBatchPath(fxl::StringView page_path,
                               fxl::StringView batch_id) {
  std::string encoded_batch_id = EncodeKey(batch_id);
  return fxl::Concatenate({page_path, kSeparator, kCommitLogCollection,
                           kSeparator, encoded_batch_id});
}

google::firestore::v1beta1::StructuredQuery MakeCommitQuery(
    std::unique_ptr<google::protobuf::Timestamp> timestamp_or_null) {
  google::firestore::v1beta1::StructuredQuery query;

  // Sub-collections to be queried.
  google::firestore::v1beta1::StructuredQuery::CollectionSelector& selector =
      *query.add_from();
  selector.set_collection_id(kCommitLogCollection);
  selector.set_all_descendants(false);

  // Ordering.
  google::firestore::v1beta1::StructuredQuery::Order& order_by =
      *query.add_order_by();
  order_by.mutable_field()->set_field_path(kTimestampField);

  // Filtering.
  if (timestamp_or_null) {
    google::firestore::v1beta1::StructuredQuery::Filter& filter =
        *query.mutable_where();
    google::firestore::v1beta1::StructuredQuery::FieldFilter& field_filter =
        *filter.mutable_field_filter();

    field_filter.mutable_field()->set_field_path(kTimestampField);
    field_filter.set_op(
        google::firestore::v1beta1::
            StructuredQuery_FieldFilter_Operator_GREATER_THAN_OR_EQUAL);
    field_filter.mutable_value()->mutable_timestamp_value()->Swap(
        timestamp_or_null.get());
  }
  return query;
}

}  // namespace

PageCloudImpl::PageCloudImpl(
    std::string page_path,
    CredentialsProvider* credentials_provider,
    FirestoreService* firestore_service,
    fidl::InterfaceRequest<cloud_provider::PageCloud> request)
    : page_path_(std::move(page_path)),
      credentials_provider_(credentials_provider),
      firestore_service_(firestore_service),
      binding_(this, std::move(request)),
      weak_ptr_factory_(this) {
  // The class shuts down when the client connection is disconnected.
  binding_.set_error_handler([this] {
    if (on_empty_) {
      on_empty_();
    }
  });
}

PageCloudImpl::~PageCloudImpl() {}

void PageCloudImpl::ScopedGetCredentials(
    std::function<void(std::shared_ptr<grpc::CallCredentials>)> callback) {
  credentials_provider_->GetCredentials(callback::MakeScoped(
      weak_ptr_factory_.GetWeakPtr(), std::move(callback)));
}

void PageCloudImpl::AddCommits(fidl::VectorPtr<cloud_provider::Commit> commits,
                               AddCommitsCallback callback) {
  auto request = google::firestore::v1beta1::CommitRequest();
  request.set_database(firestore_service_->GetDatabasePath());

  // Set the document name to a new UUID. Firestore Commit() API doesn't allow
  // to request the ID to be assigned by the server.
  const std::string document_name =
      GetCommitBatchPath(page_path_, fxl::GenerateUUID());

  // The commit batch is added in a single commit containing multiple writes.
  //
  // First write adds the document containing the encoded commit batch.
  google::firestore::v1beta1::Write& add_batch_write = *(request.add_writes());
  EncodeCommitBatch(commits, add_batch_write.mutable_update());
  (*add_batch_write.mutable_update()->mutable_name()) = document_name;
  // Ensure that the write doesn't overwrite an existing document.
  add_batch_write.mutable_current_document()->set_exists(false);

  // The second write sets the timestamp field to the server-side request
  // timestamp.
  google::firestore::v1beta1::Write& set_timestamp_write =
      *(request.add_writes());
  (*set_timestamp_write.mutable_transform()->mutable_document()) =
      document_name;

  google::firestore::v1beta1::DocumentTransform_FieldTransform& transform =
      *(set_timestamp_write.mutable_transform()->add_field_transforms());
  *(transform.mutable_field_path()) = kTimestampField;
  transform.set_set_to_server_value(
      google::firestore::v1beta1::
          DocumentTransform_FieldTransform_ServerValue_REQUEST_TIME);

  ScopedGetCredentials([this, request = std::move(request),
                        callback](auto call_credentials) mutable {
    firestore_service_->Commit(
        std::move(request), std::move(call_credentials),
        [callback](auto status, auto result) {
          if (LogGrpcRequestError(status)) {
            callback(ConvertGrpcStatus(status.error_code()));
            return;
          }
          callback(cloud_provider::Status::OK);
        });
  });
}

void PageCloudImpl::GetCommits(fidl::VectorPtr<uint8_t> min_position_token,
                               GetCommitsCallback callback) {
  std::unique_ptr<google::protobuf::Timestamp> timestamp_or_null;
  if (min_position_token) {
    timestamp_or_null = std::make_unique<google::protobuf::Timestamp>();
    if (!timestamp_or_null->ParseFromString(
            convert::ToString(min_position_token))) {
      callback(cloud_provider::Status::ARGUMENT_ERROR, nullptr, nullptr);
      return;
    }
  }

  auto request = google::firestore::v1beta1::RunQueryRequest();
  request.set_parent(page_path_);
  auto query = MakeCommitQuery(std::move(timestamp_or_null));
  request.mutable_structured_query()->Swap(&query);

  ScopedGetCredentials([this, request = std::move(request),
                        callback](auto call_credentials) mutable {
    firestore_service_->RunQuery(
        std::move(request), std::move(call_credentials),
        [callback](auto status, auto result) {
          if (LogGrpcRequestError(status)) {
            callback(ConvertGrpcStatus(status.error_code()), nullptr, nullptr);
            return;
          }

          fidl::VectorPtr<cloud_provider::Commit> commits(
              static_cast<size_t>(0u));
          std::string timestamp;

          for (const auto& response : result) {
            fidl::VectorPtr<cloud_provider::Commit> batch_commits;
            if (!response.has_document() ||
                !DecodeCommitBatch(response.document(), &batch_commits,
                                   &timestamp)) {
              callback(cloud_provider::Status::PARSE_ERROR, nullptr, nullptr);
            }

            std::move(batch_commits->begin(), batch_commits->end(),
                      std::back_inserter(*commits));
          }

          callback(cloud_provider::Status::OK, std::move(commits),
                   convert::ToArray(timestamp));
        });
  });
}

void PageCloudImpl::AddObject(fidl::VectorPtr<uint8_t> id,
                              mem::Buffer data,
                              AddObjectCallback callback) {
  std::string data_str;
  fsl::SizedVmo vmo;
  if (!fsl::StringFromVmo(data, &data_str) ||
      data_str.size() > kMaxObjectSize) {
    callback(cloud_provider::Status::ARGUMENT_ERROR);
    return;
  }

  auto request = google::firestore::v1beta1::CreateDocumentRequest();
  request.set_parent(page_path_);
  request.set_collection_id(kObjectCollection);
  google::firestore::v1beta1::Document* document = request.mutable_document();
  request.set_document_id(EncodeKey(convert::ToString(id)));
  *((*document->mutable_fields())[kDataKey].mutable_bytes_value()) =
      std::move(data_str);

  ScopedGetCredentials([this, request = std::move(request),
                        callback](auto call_credentials) mutable {
    firestore_service_->CreateDocument(
        std::move(request), std::move(call_credentials),
        [callback](auto status, auto result) {
          if (LogGrpcRequestError(status)) {
            callback(ConvertGrpcStatus(status.error_code()));
            return;
          }
          callback(cloud_provider::Status::OK);
        });
  });
}

void PageCloudImpl::GetObject(fidl::VectorPtr<uint8_t> id,
                              GetObjectCallback callback) {
  auto request = google::firestore::v1beta1::GetDocumentRequest();
  request.set_name(GetObjectPath(page_path_, convert::ToString(id)));

  ScopedGetCredentials([this, request = std::move(request),
                        callback](auto call_credentials) mutable {
    firestore_service_->GetDocument(
        std::move(request), std::move(call_credentials),
        [callback](auto status, auto result) {
          if (LogGrpcRequestError(status)) {
            callback(ConvertGrpcStatus(status.error_code()), 0u, zx::socket());
            return;
          }

          if (result.fields().count(kDataKey) != 1) {
            FXL_LOG(ERROR)
                << "Incorrect format of the retrieved object document";
            callback(cloud_provider::Status::PARSE_ERROR, 0u, zx::socket());
            return;
          }

          const std::string& bytes = result.fields().at(kDataKey).bytes_value();
          callback(cloud_provider::Status::OK, bytes.size(),
                   fsl::WriteStringToSocket(bytes));
        });
  });
}

void PageCloudImpl::SetWatcher(
    fidl::VectorPtr<uint8_t> /*min_position_token*/,
    fidl::InterfaceHandle<cloud_provider::PageCloudWatcher> /*watcher*/,
    SetWatcherCallback callback) {
  FXL_NOTIMPLEMENTED();
  callback(cloud_provider::Status::INTERNAL_ERROR);
}

}  // namespace cloud_provider_firestore
