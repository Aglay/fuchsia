// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/cloud_provider_firestore/app/device_set_impl.h"

#include <google/firestore/v1beta1/firestore.pb.h>

#include "garnet/lib/callback/scoped_callback.h"
#include "garnet/lib/callback/waiter.h"
#include "lib/fxl/logging.h"
#include "lib/fxl/strings/concatenate.h"
#include "lib/fxl/strings/string_view.h"
#include "peridot/bin/cloud_provider_firestore/app/grpc_status.h"
#include "peridot/bin/cloud_provider_firestore/firestore/encoding.h"
#include "peridot/lib/convert/convert.h"

namespace cloud_provider_firestore {

namespace {
constexpr char kSeparator[] = "/";
constexpr char kDeviceCollection[] = "devices";
constexpr char kExistsKey[] = "exists";

std::string GetDevicePath(fxl::StringView user_path,
                          fxl::StringView fingerprint) {
  std::string encoded_fingerprint = EncodeKey(fingerprint);
  return fxl::Concatenate({user_path, kSeparator, kDeviceCollection, kSeparator,
                           encoded_fingerprint});
}

class GrpcStatusAccumulator {
 public:
  bool PrepareCall() { return true; }

  bool Update(bool /*token*/, grpc::Status status) {
    result_status_ = status;
    return result_status_.ok();
  }

  grpc::Status Result() { return result_status_; }

 private:
  grpc::Status result_status_ = grpc::Status::OK;
};

class GrpcStatusWaiter : public callback::BaseWaiter<GrpcStatusAccumulator,
                                                     grpc::Status,
                                                     grpc::Status> {
 public:
  GrpcStatusWaiter()
      : callback::BaseWaiter<GrpcStatusAccumulator, grpc::Status, grpc::Status>(
            GrpcStatusAccumulator()) {}

  static fxl::RefPtr<GrpcStatusWaiter> Create() {
    return fxl::MakeRefCounted<GrpcStatusWaiter>();
  }
};
}  // namespace

DeviceSetImpl::DeviceSetImpl(
    std::string user_path,
    CredentialsProvider* credentials_provider,
    FirestoreService* firestore_service,
    f1dl::InterfaceRequest<cloud_provider::DeviceSet> request)
    : user_path_(std::move(user_path)),
      credentials_provider_(credentials_provider),
      firestore_service_(firestore_service),
      binding_(this, std::move(request)),
      weak_ptr_factory_(this) {
  FXL_DCHECK(!user_path_.empty());
  FXL_DCHECK(credentials_provider_);
  FXL_DCHECK(firestore_service_);

  // The class shuts down when the client connection is disconnected.
  binding_.set_error_handler([this] {
    if (on_empty_) {
      on_empty_();
    }
  });
}

DeviceSetImpl::~DeviceSetImpl() {}

void DeviceSetImpl::CheckFingerprint(f1dl::VectorPtr<uint8_t> fingerprint,
                                     const CheckFingerprintCallback& callback) {
  auto request = google::firestore::v1beta1::GetDocumentRequest();
  request.set_name(
      GetDevicePath(user_path_, convert::ToStringView(fingerprint)));

  credentials_provider_->GetCredentials(
      [this, request = std::move(request),
       callback](auto call_credentials) mutable {
        firestore_service_->GetDocument(
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

void DeviceSetImpl::SetFingerprint(f1dl::VectorPtr<uint8_t> fingerprint,
                                   const SetFingerprintCallback& callback) {
  auto request = google::firestore::v1beta1::CreateDocumentRequest();
  request.set_parent(user_path_);
  request.set_collection_id(kDeviceCollection);
  request.set_document_id(EncodeKey(convert::ToString(fingerprint)));
  google::firestore::v1beta1::Value exists;
  // TODO(ppi): store a timestamp of the last connection rather than a boolean
  // flag.
  exists.set_boolean_value(true);
  (*(request.mutable_document()->mutable_fields()))[kExistsKey] = exists;

  credentials_provider_->GetCredentials(
      [this, request = std::move(request),
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

void DeviceSetImpl::SetWatcher(
    f1dl::VectorPtr<uint8_t> fingerprint,
    f1dl::InterfaceHandle<cloud_provider::DeviceSetWatcher> watcher,
    const SetWatcherCallback& callback) {
  watcher_ = watcher.Bind();
  watched_fingerprint_ = convert::ToString(fingerprint);
  set_watcher_callback_ = callback;

  credentials_provider_->GetCredentials([this](auto call_credentials) mutable {
    // Initiate the listen RPC. We will receive a call on OnConnected() when the
    // watcher is ready.
    listen_call_handler_ =
        firestore_service_->Listen(std::move(call_credentials), this);
  });
}

void DeviceSetImpl::Erase(const EraseCallback& callback) {
  auto request = google::firestore::v1beta1::ListDocumentsRequest();
  request.set_parent(user_path_);
  request.set_collection_id(kDeviceCollection);

  credentials_provider_->GetCredentials(
      [this, request = std::move(request),
       callback = callback](auto call_credentials) mutable {
        firestore_service_->ListDocuments(
            std::move(request), call_credentials,
            [this, call_credentials, callback = std::move(callback)](
                auto status, auto result) mutable {
              if (LogGrpcRequestError(status)) {
                callback(ConvertGrpcStatus(status.error_code()));
                return;
              }
              OnGotDocumentsToErase(std::move(call_credentials),
                                    std::move(result), std::move(callback));
            });
      });
}

void DeviceSetImpl::OnGotDocumentsToErase(
    std::shared_ptr<grpc::CallCredentials> call_credentials,
    google::firestore::v1beta1::ListDocumentsResponse documents_response,
    EraseCallback callback) {
  if (!documents_response.next_page_token().empty()) {
    // TODO(ppi): handle paginated response.
    FXL_LOG(ERROR)
        << "Failed to erase the device map - too many devices in the map.";
    callback(cloud_provider::Status::INTERNAL_ERROR);
    return;
  }

  auto waiter = GrpcStatusWaiter::Create();
  for (const auto& document : documents_response.documents()) {
    auto request = google::firestore::v1beta1::DeleteDocumentRequest();
    request.set_name(document.name());
    firestore_service_->DeleteDocument(std::move(request), call_credentials,
                                       waiter->NewCallback());
  }
  waiter->Finalize(callback::MakeScoped(
      weak_ptr_factory_.GetWeakPtr(),
      [callback = std::move(callback)](grpc::Status status) {
        if (LogGrpcRequestError(status)) {
          callback(ConvertGrpcStatus(status.error_code()));
          return;
        }
        callback(cloud_provider::Status::OK);
      }));
}

void DeviceSetImpl::OnConnected() {
  auto request = google::firestore::v1beta1::ListenRequest();
  request.set_database(firestore_service_->GetDatabasePath());
  request.mutable_add_target()->mutable_documents()->add_documents(
      GetDevicePath(user_path_, watched_fingerprint_));
  listen_call_handler_->Write(std::move(request));
}

void DeviceSetImpl::OnResponse(
    google::firestore::v1beta1::ListenResponse response) {
  if (response.has_target_change()) {
    if (response.target_change().target_change_type() ==
        google::firestore::v1beta1::TargetChange_TargetChangeType_CURRENT) {
      if (set_watcher_callback_) {
        set_watcher_callback_(cloud_provider::Status::OK);
        set_watcher_callback_ = nullptr;
      }
    }
    return;
  }
  if (response.has_document_delete()) {
    if (set_watcher_callback_) {
      set_watcher_callback_(cloud_provider::Status::NOT_FOUND);
      set_watcher_callback_ = nullptr;
    }
    watcher_->OnCloudErased();
    return;
  }
}

void DeviceSetImpl::OnFinished(grpc::Status status) {
  if (status.error_code() == grpc::UNAVAILABLE ||
      status.error_code() == grpc::UNAUTHENTICATED) {
    if (watcher_) {
      watcher_->OnNetworkError();
    }
    return;
  }
  LogGrpcConnectionError(status);
  watcher_.Unbind();
}

}  // namespace cloud_provider_firestore
