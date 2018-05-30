// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/ledger/p2p_sync/impl/page_communicator_impl.h"

#include "lib/callback/scoped_callback.h"
#include "lib/callback/waiter.h"
#include "peridot/bin/ledger/p2p_sync/impl/message_generated.h"
#include "peridot/bin/ledger/storage/public/read_data_source.h"
#include "peridot/lib/convert/convert.h"

namespace p2p_sync {
namespace {
storage::ObjectIdentifier ToObjectIdentifier(const ObjectId* fb_object_id) {
  uint32_t key_index = fb_object_id->key_index();
  uint32_t deletion_scope_id = fb_object_id->deletion_scope_id();
  return storage::ObjectIdentifier{key_index, deletion_scope_id,
                                   convert::ToString(fb_object_id->digest())};
}
}  // namespace

// PendingObjectRequestHolder holds state for object requests that have been
// sent to peers and for which we wait for an answer.
class PageCommunicatorImpl::PendingObjectRequestHolder {
 public:
  explicit PendingObjectRequestHolder(
      std::function<void(storage::Status, storage::ChangeSource,
                         std::unique_ptr<storage::DataSource::DataChunk>)>
          callback)
      : callback_(std::move(callback)) {}

  void set_on_empty(fxl::Closure on_empty) { on_empty_ = std::move(on_empty); }

  // Registers a new pending request to device |destination|.
  void AddNewPendingRequest(std::string destination) {
    requests_.emplace(std::move(destination));
  }

  // Processes the response from device |source|.
  void Complete(fxl::StringView source, const Object* object) {
    auto it = requests_.find(source);
    if (it == requests_.end()) {
      return;
    }
    if (object == nullptr || object->status() == ObjectStatus_UNKNOWN_OBJECT) {
      requests_.erase(it);
      if (!requests_.empty()) {
        return;
      }
      // All requests have returned and none is valid: return an error.
      callback_(storage::Status::NOT_FOUND, storage::ChangeSource::P2P,
                nullptr);
      if (on_empty_) {
        on_empty_();
      }
      return;
    }

    std::unique_ptr<storage::DataSource::DataChunk> chunk =
        storage::DataSource::DataChunk::Create(
            convert::ToString(object->data()->bytes()));
    callback_(storage::Status::OK, storage::ChangeSource::P2P,
              std::move(chunk));
    if (on_empty_) {
      on_empty_();
    }
  }

 private:
  std::function<void(storage::Status, storage::ChangeSource,
                     std::unique_ptr<storage::DataSource::DataChunk>)> const
      callback_;
  // Set of devices for which we are waiting an answer.
  // We might be able to get rid of this list and just use a counter (or even
  // nothing at all) once we have a timeout on requests.
  std::set<std::string, convert::StringViewComparator> requests_;
  fxl::Closure on_empty_;
};

PageCommunicatorImpl::PageCommunicatorImpl(storage::PageStorage* storage,
                                           storage::PageSyncClient* sync_client,
                                           std::string namespace_id,
                                           std::string page_id,
                                           DeviceMesh* mesh)
    : namespace_id_(std::move(namespace_id)),
      page_id_(std::move(page_id)),
      mesh_(mesh),
      storage_(storage),
      sync_client_(sync_client),
      weak_factory_(this) {}

PageCommunicatorImpl::~PageCommunicatorImpl() {
  FXL_DCHECK(!in_destructor_);
  in_destructor_ = true;

  flatbuffers::FlatBufferBuilder buffer;
  if (!started_) {
    if (on_delete_) {
      on_delete_();
    }
    return;
  }

  BuildWatchStopBuffer(&buffer);
  char* buf = reinterpret_cast<char*>(buffer.GetBufferPointer());
  size_t size = buffer.GetSize();

  for (const auto& device : interested_devices_) {
    mesh_->Send(device, fxl::StringView(buf, size));
  }

  if (on_delete_) {
    on_delete_();
  }
}

void PageCommunicatorImpl::Start() {
  FXL_DCHECK(!started_);
  started_ = true;
  sync_client_->SetSyncDelegate(this);

  flatbuffers::FlatBufferBuilder buffer;
  BuildWatchStartBuffer(&buffer);

  for (const auto& device : mesh_->GetDeviceList()) {
    mesh_->Send(device, convert::ExtendedStringView(buffer));
  }
}

void PageCommunicatorImpl::set_on_delete(fxl::Closure on_delete) {
  FXL_DCHECK(!on_delete_) << "set_on_delete() can only be called once.";
  on_delete_ = std::move(on_delete);
}

void PageCommunicatorImpl::OnDeviceChange(
    fxl::StringView remote_device, p2p_provider::DeviceChangeType change_type) {
  if (!started_ || in_destructor_) {
    return;
  }

  if (change_type == p2p_provider::DeviceChangeType::DELETED) {
    const auto& it = interested_devices_.find(remote_device);
    if (it != interested_devices_.end()) {
      interested_devices_.erase(it);
    }
    const auto& it2 = not_interested_devices_.find(remote_device);
    if (it2 != not_interested_devices_.end()) {
      not_interested_devices_.erase(it2);
    }
    return;
  }

  flatbuffers::FlatBufferBuilder buffer;
  BuildWatchStartBuffer(&buffer);
  mesh_->Send(remote_device, convert::ExtendedStringView(buffer));
}

void PageCommunicatorImpl::OnNewRequest(fxl::StringView source,
                                        const Request* message) {
  FXL_DCHECK(!in_destructor_);
  switch (message->request_type()) {
    case RequestMessage_WatchStartRequest: {
      if (interested_devices_.find(source) == interested_devices_.end()) {
        interested_devices_.insert(source.ToString());
      }
      auto it = not_interested_devices_.find(source);
      if (it != not_interested_devices_.end()) {
        // The device used to be ininterested, but now wants updates. Let's
        // contact it again.
        not_interested_devices_.erase(it);
        flatbuffers::FlatBufferBuilder buffer;
        BuildWatchStartBuffer(&buffer);
        mesh_->Send(source, convert::ExtendedStringView(buffer));
      }
      break;
    }
    case RequestMessage_WatchStopRequest: {
      const auto& it = interested_devices_.find(source);
      if (it != interested_devices_.end()) {
        interested_devices_.erase(it);
      }
      // Device |source| disconnected, thus will not answer any request. We thus
      // mark all pending requests to |source| to be finished.
      for (auto& object_request : pending_object_requests_) {
        object_request.second.Complete(source, nullptr);
      }
      break;
    }
    case RequestMessage_CommitRequest:
      FXL_NOTIMPLEMENTED();
      break;
    case RequestMessage_ObjectRequest:
      ProcessObjectRequest(
          source, static_cast<const ObjectRequest*>(message->request()));
      break;
    case RequestMessage_NONE:
      FXL_LOG(ERROR) << "The message received is malformed: " << message;
      break;
  }
}

void PageCommunicatorImpl::OnNewResponse(fxl::StringView source,
                                         const Response* message) {
  FXL_DCHECK(!in_destructor_);
  if (message->status() != ResponseStatus_OK) {
    // The namespace or page was unknown on the other side. We can probably do
    // something smart with this information (for instance, stop sending
    // requests over), but we just ignore it for now.
    not_interested_devices_.emplace(source.ToString());
    return;
  }
  switch (message->response_type()) {
    case ResponseMessage_ObjectResponse: {
      const ObjectResponse* object_response =
          static_cast<const ObjectResponse*>(message->response());
      for (const Object* object : *(object_response->objects())) {
        auto object_id = ToObjectIdentifier(object->id());
        auto pending_request = pending_object_requests_.find(object_id);
        if (pending_request == pending_object_requests_.end()) {
          continue;
        }
        pending_request->second.Complete(source, object);
      }
      break;
    }
    case ResponseMessage_CommitResponse: {
      FXL_NOTIMPLEMENTED();
      break;
    }
    case ResponseMessage_NONE:
      FXL_LOG(ERROR) << "The message received is malformed: " << message;
      return;
  }
}

void PageCommunicatorImpl::GetObject(
    storage::ObjectIdentifier object_identifier,
    std::function<void(storage::Status, storage::ChangeSource,
                       std::unique_ptr<storage::DataSource::DataChunk>)>
        callback) {
  flatbuffers::FlatBufferBuilder buffer;

  BuildObjectRequestBuffer(&buffer, object_identifier);
  char* buf = reinterpret_cast<char*>(buffer.GetBufferPointer());
  size_t size = buffer.GetSize();

  auto request_holder = pending_object_requests_.emplace(
      std::move(object_identifier), std::move(callback));

  for (const auto& device : interested_devices_) {
    mesh_->Send(device, fxl::StringView(buf, size));
    request_holder.first->second.AddNewPendingRequest(device);
  }
}

void PageCommunicatorImpl::BuildWatchStartBuffer(
    flatbuffers::FlatBufferBuilder* buffer) {
  flatbuffers::Offset<NamespacePageId> namespace_page_id =
      CreateNamespacePageId(*buffer,
                            convert::ToFlatBufferVector(buffer, namespace_id_),
                            convert::ToFlatBufferVector(buffer, page_id_));
  flatbuffers::Offset<Request> request = CreateRequest(
      *buffer, namespace_page_id, RequestMessage_WatchStartRequest);
  flatbuffers::Offset<Message> message =
      CreateMessage(*buffer, MessageUnion_Request, request.Union());
  buffer->Finish(message);
}

void PageCommunicatorImpl::BuildWatchStopBuffer(
    flatbuffers::FlatBufferBuilder* buffer) {
  flatbuffers::Offset<NamespacePageId> namespace_page_id =
      CreateNamespacePageId(*buffer,
                            convert::ToFlatBufferVector(buffer, namespace_id_),
                            convert::ToFlatBufferVector(buffer, page_id_));
  flatbuffers::Offset<Request> request = CreateRequest(
      *buffer, namespace_page_id, RequestMessage_WatchStopRequest);
  flatbuffers::Offset<Message> message =
      CreateMessage(*buffer, MessageUnion_Request, request.Union());
  buffer->Finish(message);
}

void PageCommunicatorImpl::BuildObjectRequestBuffer(
    flatbuffers::FlatBufferBuilder* buffer,
    storage::ObjectIdentifier object_identifier) {
  flatbuffers::Offset<NamespacePageId> namespace_page_id =
      CreateNamespacePageId(*buffer,
                            convert::ToFlatBufferVector(buffer, namespace_id_),
                            convert::ToFlatBufferVector(buffer, page_id_));
  flatbuffers::Offset<ObjectId> object_id = CreateObjectId(
      *buffer, object_identifier.key_index, object_identifier.deletion_scope_id,
      convert::ToFlatBufferVector(buffer, object_identifier.object_digest));
  flatbuffers::Offset<ObjectRequest> object_request = CreateObjectRequest(
      *buffer, buffer->CreateVector(
                   std::vector<flatbuffers::Offset<ObjectId>>({object_id})));
  flatbuffers::Offset<Request> request =
      CreateRequest(*buffer, namespace_page_id, RequestMessage_ObjectRequest,
                    object_request.Union());
  flatbuffers::Offset<Message> message =
      CreateMessage(*buffer, MessageUnion_Request, request.Union());
  buffer->Finish(message);
}

void PageCommunicatorImpl::ProcessObjectRequest(fxl::StringView source,
                                                const ObjectRequest* request) {
  auto waiter = callback::Waiter<
      bool, std::pair<storage::ObjectIdentifier,
                      std::unique_ptr<const storage::Object>>>::Create(true);
  for (const ObjectId* object_id : *request->object_ids()) {
    storage::ObjectIdentifier identifier{
        object_id->key_index(), object_id->deletion_scope_id(),
        convert::ExtendedStringView(object_id->digest()).ToString()};
    auto callback = waiter->NewCallback();
    auto get_piece_callback =
        [callback = std::move(callback), identifier](
            storage::Status status,
            std::unique_ptr<const storage::Object> object) mutable {
          if (status != storage::Status::OK) {
            // Not finding an object is okay in this context: we'll just reply
            // we don't have it. There is not need to abort processing the
            // request.
            callback(true, std::make_pair(std::move(identifier), nullptr));
            return;
          }
          callback(true,
                   std::make_pair(std::move(identifier), std::move(object)));
        };
    storage_->GetPiece(std::move(identifier), std::move(get_piece_callback));
  }

  waiter->Finalize(callback::MakeScoped(
      weak_factory_.GetWeakPtr(),
      [this, source = source.ToString()](
          bool status,
          std::vector<std::pair<storage::ObjectIdentifier,
                                std::unique_ptr<const storage::Object>>>
              results) mutable {
        // We always return a true |status| to not abort early. See also the
        // comment above.
        FXL_DCHECK(status);
        flatbuffers::FlatBufferBuilder buffer;
        BuildObjectResponseBuffer(&buffer, std::move(results));
        char* buf = reinterpret_cast<char*>(buffer.GetBufferPointer());
        size_t size = buffer.GetSize();

        mesh_->Send(source, fxl::StringView(buf, size));
      }));
}

void PageCommunicatorImpl::BuildObjectResponseBuffer(
    flatbuffers::FlatBufferBuilder* buffer,
    std::vector<std::pair<storage::ObjectIdentifier,
                          std::unique_ptr<const storage::Object>>>
        results) {
  flatbuffers::Offset<NamespacePageId> namespace_page_id =
      CreateNamespacePageId(*buffer,
                            convert::ToFlatBufferVector(buffer, namespace_id_),
                            convert::ToFlatBufferVector(buffer, page_id_));
  std::vector<flatbuffers::Offset<Object>> fb_objects;
  for (const auto& object_pair : results) {
    flatbuffers::Offset<ObjectId> fb_object_id = CreateObjectId(
        *buffer, object_pair.first.key_index,
        object_pair.first.deletion_scope_id,
        convert::ToFlatBufferVector(buffer, object_pair.first.object_digest));
    if (object_pair.second) {
      fxl::StringView data;
      storage::Status status = object_pair.second->GetData(&data);
      if (status != storage::Status::OK) {
        FXL_LOG(ERROR) << "Unable to read object data, aborting: " << status;
        // We do getData first so that we can continue in case of error
        // without having written anything in the flatbuffer.
        continue;
      }
      flatbuffers::Offset<Data> fb_data =
          CreateData(*buffer, convert::ToFlatBufferVector(buffer, data));
      fb_objects.emplace_back(
          CreateObject(*buffer, fb_object_id, ObjectStatus_OK, fb_data));
    } else {
      fb_objects.emplace_back(
          CreateObject(*buffer, fb_object_id, ObjectStatus_UNKNOWN_OBJECT));
    }
  }
  flatbuffers::Offset<ObjectResponse> object_response =
      CreateObjectResponse(*buffer, buffer->CreateVector(fb_objects));
  flatbuffers::Offset<Response> response =
      CreateResponse(*buffer, ResponseStatus_OK, namespace_page_id,
                     ResponseMessage_ObjectResponse, object_response.Union());
  flatbuffers::Offset<Message> message =
      CreateMessage(*buffer, MessageUnion_Response, response.Union());
  buffer->Finish(message);
}

}  // namespace p2p_sync
