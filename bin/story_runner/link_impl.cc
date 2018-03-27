// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/story_runner/link_impl.h"

#include <functional>

#include "lib/fidl/cpp/bindings/interface_handle.h"
#include "lib/fidl/cpp/interface_request.h"
#include "lib/fxl/logging.h"
#include "lib/story/fidl/create_link.fidl.h"
#include "lib/story/fidl/link.fidl.h"
#include "peridot/lib/fidl/json_xdr.h"
#include "peridot/lib/ledger_client/operations.h"
#include "peridot/lib/ledger_client/storage.h"
#include "peridot/lib/rapidjson/rapidjson.h"
#include "peridot/lib/util/debug.h"
#include "peridot/public/lib/entity/cpp/json.h"

namespace modular {

class LinkImpl::ReadLinkDataCall : Operation<f1dl::StringPtr> {
 public:
  ReadLinkDataCall(OperationContainer* const container,
                   ledger::Page* const page,
                   const LinkPathPtr& link_path,
                   ResultCall result_call)
      : Operation("LinkImpl::ReadLinkDataCall",
                  container,
                  std::move(result_call)),
        page_(page),
        link_key_(MakeLinkKey(link_path)) {
    Ready();
  }

 private:
  void Run() override {
    FlowToken flow{this, &result_};

    page_->GetSnapshot(page_snapshot_.NewRequest(), nullptr, nullptr,
                       [this, flow](ledger::Status status) {
                         if (status != ledger::Status::OK) {
                           FXL_LOG(ERROR)
                               << trace_name() << " " << link_key_ << " "
                               << " Page.GetSnapshot() " << status;
                           return;
                         }

                         Cont(flow);
                       });
  }

  void Cont(FlowToken flow) {
    page_snapshot_->Get(
        to_array(link_key_),
        [this, flow](ledger::Status status, fsl::SizedVmoTransportPtr value) {
          if (status != ledger::Status::OK) {
            if (status != ledger::Status::KEY_NOT_FOUND) {
              // It's expected that the key is not found when the link is
              // accessed for the first time. Don't log an error then.
              FXL_LOG(ERROR) << trace_name() << " " << link_key_ << " "
                             << " PageSnapshot.Get() " << status;
            }
            return;
          }

          std::string value_as_string;
          if (value) {
            if (!fsl::StringFromVmo(value, &value_as_string)) {
              FXL_LOG(ERROR) << trace_name() << " " << link_key_ << " "
                             << "VMO could not be copied.";
              return;
            }
          }

          result_.reset(value_as_string);
        });
  }

  ledger::Page* const page_;  // not owned
  ledger::PageSnapshotPtr page_snapshot_;
  const std::string link_key_;
  f1dl::StringPtr result_;

  FXL_DISALLOW_COPY_AND_ASSIGN(ReadLinkDataCall);
};

class LinkImpl::WriteLinkDataCall : Operation<> {
 public:
  WriteLinkDataCall(OperationContainer* const container,
                    ledger::Page* const page,
                    const LinkPathPtr& link_path,
                    f1dl::StringPtr data,
                    ResultCall result_call)
      : Operation("LinkImpl::WriteLinkDataCall",
                  container,
                  std::move(result_call)),
        page_(page),
        link_key_(MakeLinkKey(link_path)),
        data_(data) {
    Ready();
  }

 private:
  void Run() override {
    FlowToken flow{this};

    page_->Put(to_array(link_key_), to_array(data_),
               [this, flow](ledger::Status status) {
                 if (status != ledger::Status::OK) {
                   FXL_LOG(ERROR) << trace_name() << " " << link_key_ << " "
                                  << " Page.Put() " << status;
                 }
               });
  }

  ledger::Page* const page_;  // not owned
  const std::string link_key_;
  f1dl::StringPtr data_;

  FXL_DISALLOW_COPY_AND_ASSIGN(WriteLinkDataCall);
};

class LinkImpl::FlushWatchersCall : Operation<> {
 public:
  FlushWatchersCall(OperationContainer* const container,
                    ledger::Page* const page,
                    ResultCall result_call)
      : Operation("LinkImpl::FlushWatchersCall",
                  container,
                  std::move(result_call)),
        page_(page) {
    Ready();
  }

 private:
  void Run() override {
    FlowToken flow{this};

    // Cf. the documentation in ledger.fidl: Before StartTransaction() returns,
    // all pending watcher notifications on the same connection are guaranteed
    // to have returned. If we execute this Operation after a WriteLinkData()
    // call, then all link watcher notifications are guaranteed to have been
    // received when this Operation is Done().

    page_->StartTransaction([this, flow](ledger::Status status) {
      if (status != ledger::Status::OK) {
        FXL_LOG(ERROR) << trace_name() << " "
                       << " Page.StartTransaction() " << status;
        return;
      }

      page_->Commit([this, flow](ledger::Status status) {
        if (status != ledger::Status::OK) {
          FXL_LOG(ERROR) << trace_name() << " "
                         << " Page.Commit() " << status;
          return;
        }
      });
    });
  }

  ledger::Page* const page_;  // not owned

  FXL_DISALLOW_COPY_AND_ASSIGN(FlushWatchersCall);
};

class LinkImpl::ReadCall : Operation<> {
 public:
  ReadCall(OperationContainer* const container,
           LinkImpl* const impl,
           ResultCall result_call)
      : Operation("LinkImpl::ReadCall", container, std::move(result_call)),
        impl_(impl) {
    Ready();
  }

 private:
  void Run() override {
    FlowToken flow{this};
    new ReadLinkDataCall(&operation_queue_, impl_->page(), impl_->link_path_,
                         [this, flow](const f1dl::StringPtr& json) {
                           if (!json.is_null()) {
                             impl_->doc_.Parse(json.get());
                           }
                         });
  }

  LinkImpl* const impl_;  // not owned
  OperationQueue operation_queue_;

  FXL_DISALLOW_COPY_AND_ASSIGN(ReadCall);
};

class LinkImpl::WriteCall : Operation<> {
 public:
  WriteCall(OperationContainer* const container,
            LinkImpl* const impl,
            const uint32_t src,
            ResultCall result_call)
      : Operation("LinkImpl::WriteCall", container, std::move(result_call)),
        impl_(impl),
        src_(src) {
    Ready();
  }

 private:
  void Run() override {
    FlowToken flow{this};
    new WriteLinkDataCall(&operation_queue_, impl_->page(), impl_->link_path_,
                          JsonValueToString(impl_->doc_),
                          [this, flow] { Cont1(flow); });
  }

  void Cont1(FlowToken flow) {
    new FlushWatchersCall(&operation_queue_, impl_->page(),
                          [this, flow] { Cont2(flow); });
  }

  void Cont2(FlowToken /*flow*/) { impl_->NotifyWatchers(src_); }

  LinkImpl* const impl_;  // not owned
  const uint32_t src_;
  OperationQueue operation_queue_;

  FXL_DISALLOW_COPY_AND_ASSIGN(WriteCall);
};

class LinkImpl::SetSchemaCall : Operation<> {
 public:
  SetSchemaCall(OperationContainer* const container,
                LinkImpl* const impl,
                const f1dl::StringPtr& json_schema)
      : Operation("LinkImpl::SetSchemaCall", container, [] {}),
        impl_(impl),
        json_schema_(json_schema) {
    Ready();
  }

 private:
  void Run() override {
    FlowToken flow{this};

    rapidjson::Document doc;
    doc.Parse(json_schema_.get());
    if (doc.HasParseError()) {
      FXL_LOG(ERROR) << trace_name() << " " << EncodeLinkPath(impl_->link_path_)
                     << " JSON parse failed error #" << doc.GetParseError()
                     << std::endl
                     << json_schema_;
      return;
    }

    impl_->schema_doc_ = std::make_unique<rapidjson::SchemaDocument>(doc);
  }

  LinkImpl* const impl_;  // not owned
  const f1dl::StringPtr json_schema_;

  FXL_DISALLOW_COPY_AND_ASSIGN(SetSchemaCall);
};

class LinkImpl::GetCall : Operation<f1dl::StringPtr> {
 public:
  GetCall(OperationContainer* const container,
          LinkImpl* const impl,
          f1dl::VectorPtr<f1dl::StringPtr> path,
          ResultCall result_call)
      : Operation("LinkImpl::GetCall", container, std::move(result_call)),
        impl_(impl),
        path_(std::move(path)) {
    Ready();
  }

 private:
  void Run() override {
    FlowToken flow{this, &result_};

    auto p = CreatePointer(impl_->doc_, *path_).Get(impl_->doc_);

    if (p != nullptr) {
      result_ = f1dl::StringPtr(JsonValueToString(*p));
    }
  }

  LinkImpl* const impl_;  // not owned
  const f1dl::VectorPtr<f1dl::StringPtr> path_;
  f1dl::StringPtr result_;

  FXL_DISALLOW_COPY_AND_ASSIGN(GetCall);
};

class LinkImpl::SetCall : Operation<> {
 public:
  SetCall(OperationContainer* const container,
          LinkImpl* const impl,
          f1dl::VectorPtr<f1dl::StringPtr> path,
          const f1dl::StringPtr& json,
          const uint32_t src)
      : Operation("LinkImpl::SetCall", container, [] {}),
        impl_(impl),
        path_(std::move(path)),
        json_(json),
        src_(src) {
    Ready();
  }

 private:
  void Run() override {
    FlowToken flow{this};

    if (impl_->IsClientReadOnly(src_)) {
      FXL_LOG(WARNING) << "LinkImpl::SetCall failed, no write access";
      return;
    }

    CrtJsonPointer ptr = CreatePointer(impl_->doc_, *path_);
    const bool success = impl_->ApplySetOp(ptr, json_);
    if (success) {
      impl_->ValidateSchema("LinkImpl::SetCall", ptr, json_);
      new WriteCall(&operation_queue_, impl_, src_, [flow] {});
      impl_->NotifyWatchers(src_);
    } else {
      FXL_LOG(WARNING) << "LinkImpl::SetCall failed " << json_;
    }
  }

  LinkImpl* const impl_;  // not owned
  const f1dl::VectorPtr<f1dl::StringPtr> path_;
  const f1dl::StringPtr json_;
  const uint32_t src_;

  // WriteCall is executed here.
  OperationQueue operation_queue_;

  FXL_DISALLOW_COPY_AND_ASSIGN(SetCall);
};

class LinkImpl::UpdateObjectCall : Operation<> {
 public:
  UpdateObjectCall(OperationContainer* const container,
                   LinkImpl* const impl,
                   f1dl::VectorPtr<f1dl::StringPtr> path,
                   const f1dl::StringPtr& json,
                   const uint32_t src)
      : Operation("LinkImpl::UpdateObjectCall", container, [] {}),
        impl_(impl),
        path_(std::move(path)),
        json_(json),
        src_(src) {
    Ready();
  }

 private:
  void Run() override {
    FlowToken flow{this};

    if (impl_->IsClientReadOnly(src_)) {
      FXL_LOG(WARNING) << "LinkImpl::UpdateObjectCall failed, no write access";
      return;
    }

    CrtJsonPointer ptr = CreatePointer(impl_->doc_, *path_);
    const bool success = impl_->ApplyUpdateOp(ptr, json_);
    if (success) {
      impl_->ValidateSchema("LinkImpl::UpdateObject", ptr, json_);
      new WriteCall(&operation_queue_, impl_, src_, [flow] {});
      impl_->NotifyWatchers(src_);
    } else {
      FXL_LOG(WARNING) << "LinkImpl::UpdateObjectCall failed " << json_;
    }
  }

  LinkImpl* const impl_;  // not owned
  const f1dl::VectorPtr<f1dl::StringPtr> path_;
  const f1dl::StringPtr json_;
  const uint32_t src_;

  // WriteCall is executed here.
  OperationQueue operation_queue_;

  FXL_DISALLOW_COPY_AND_ASSIGN(UpdateObjectCall);
};

class LinkImpl::EraseCall : Operation<> {
 public:
  EraseCall(OperationContainer* const container,
            LinkImpl* const impl,
            f1dl::VectorPtr<f1dl::StringPtr> path,
            const uint32_t src)
      : Operation("LinkImpl::EraseCall", container, [] {}),
        impl_(impl),
        path_(std::move(path)),
        src_(src) {
    Ready();
  }

 private:
  void Run() override {
    FlowToken flow{this};

    if (impl_->IsClientReadOnly(src_)) {
      FXL_LOG(WARNING) << "LinkImpl::EraseCall failed, no write access";
      return;
    }

    CrtJsonPointer ptr = CreatePointer(impl_->doc_, *path_);
    const bool success = impl_->ApplyEraseOp(ptr);
    if (success) {
      impl_->ValidateSchema("LinkImpl::EraseCall", ptr, std::string());
      new WriteCall(&operation_queue_, impl_, src_, [flow] {});
      impl_->NotifyWatchers(src_);
    } else {
      FXL_LOG(WARNING) << "LinkImpl::EraseCall failed ";
    }
  }

  LinkImpl* const impl_;  // not owned
  const f1dl::VectorPtr<f1dl::StringPtr> path_;
  const uint32_t src_;

  // WriteCall is executed here.
  OperationQueue operation_queue_;

  FXL_DISALLOW_COPY_AND_ASSIGN(EraseCall);
};

class LinkImpl::GetEntityCall : Operation<f1dl::StringPtr> {
 public:
  GetEntityCall(OperationContainer* const container,
                LinkImpl* const impl,
                ResultCall result_call)
      : Operation("LinkImpl::GetEntityCall", container, std::move(result_call)),
        impl_(impl) {
    Ready();
  }

 private:
  void Run() override {
    FlowToken flow{this, &result_};
    new GetCall(&operation_queue_, impl_, f1dl::VectorPtr<f1dl::StringPtr>::New(0),
                [this, flow](const f1dl::StringPtr& value) {
                  Cont(std::move(flow), value);
                });
  }

  void Cont(FlowToken flow, const f1dl::StringPtr& json) {
    std::string entity_reference;
    result_.reset();
    if (EntityReferenceFromJson(json, &entity_reference)) {
      result_.reset(std::move(entity_reference));
    }
  }

  LinkImpl* const impl_;  // not owned
  f1dl::StringPtr result_;

  // GetCall is executed here.
  OperationQueue operation_queue_;

  FXL_DISALLOW_COPY_AND_ASSIGN(GetEntityCall);
};

class LinkImpl::WatchCall : Operation<> {
 public:
  WatchCall(OperationContainer* const container,
            LinkImpl* const impl,
            f1dl::InterfaceHandle<LinkWatcher> watcher,
            const uint32_t conn)
      : Operation("LinkImpl::WatchCall", container, [] {}),
        impl_(impl),
        watcher_(watcher.Bind()),
        conn_(conn) {
    Ready();
  }

 private:
  void Run() override {
    FlowToken flow{this};

    // TODO(jimbe): We need to send an initial notification of state until there
    // is snapshot information that can be used by clients to query the state at
    // this instant. Otherwise there is no sequence information about total
    // state versus incremental changes.
    //
    // TODO(mesch): We should adopt the pattern from ledger to read the value
    // and register a watcher for subsequent changes in the same operation, so
    // that we don't have to send the current value to the watcher.
    watcher_->Notify(JsonValueToString(impl_->doc_));

    impl_->watchers_.emplace_back(std::make_unique<LinkWatcherConnection>(
        impl_, std::move(watcher_), conn_));
  }

  LinkImpl* const impl_;  // not owned
  LinkWatcherPtr watcher_;
  const uint32_t conn_;

  FXL_DISALLOW_COPY_AND_ASSIGN(WatchCall);
};

class LinkImpl::ChangeCall : Operation<> {
 public:
  ChangeCall(OperationContainer* const container,
             LinkImpl* const impl,
             const f1dl::StringPtr& json)
      : Operation("LinkImpl::ChangeCall", container, [] {}),
        impl_(impl),
        json_(json) {
    Ready();
  }

 private:
  void Run() override {
    FlowToken flow{this};

    // NOTE(jimbe) With rapidjson, the opposite check is more expensive, O(n^2),
    // so we won't do it for now. See case kObjectType in operator==() in
    // include/rapidjson/document.h.
    //
    //  if (doc_.Equals(json)) {
    //    return;
    //  }
    //
    // Since all json in a link was written by the same serializer, this check
    // is mostly accurate. This test has false negatives when only order
    // differs.
    if (json_ == JsonValueToString(impl_->doc_)) {
      return;
    }

    impl_->doc_.Parse(json_);
    impl_->NotifyWatchers(kOnChangeConnectionId);
  }

  LinkImpl* const impl_;  // not owned
  const f1dl::StringPtr json_;

  FXL_DISALLOW_COPY_AND_ASSIGN(ChangeCall);
};

LinkImpl::LinkImpl(LedgerClient* const ledger_client,
                   LedgerPageId page_id,
                   LinkPathPtr link_path,
                   CreateLinkInfoPtr create_link_info)
    : PageClient(MakeLinkKey(link_path),
                 ledger_client,
                 std::move(page_id),
                 MakeLinkKey(link_path)),
      link_path_(std::move(link_path)),
      create_link_info_(std::move(create_link_info)) {
  MakeReloadCall([this] {
    // Convert the indices of the pending primary handles into connection IDs
    // and store the connection IDs in a set<> that's quickly searchable.
    for (auto index : requests_primary_indices_) {
      primary_connection_ids_.insert(index + next_connection_id_);
    }
    for (auto& request : requests_) {
      LinkConnection::New(this, next_connection_id_++, std::move(request));
    }
    requests_.clear();
    requests_primary_indices_.clear();
    ready_ = true;
  });
}

LinkImpl::~LinkImpl() = default;

void LinkImpl::Connect(f1dl::InterfaceRequest<Link> request,
                       ConnectionType connection_type) {
  if (ready_) {
    if (connection_type == ConnectionType::Primary) {
      primary_connection_ids_.insert(next_connection_id_);
    }
    LinkConnection::New(this, next_connection_id_++, std::move(request));
  } else {
    if (connection_type == ConnectionType::Primary) {
      requests_primary_indices_.push_back(requests_.size());
    }
    requests_.emplace_back(std::move(request));
  }
}

void LinkImpl::SetSchema(const f1dl::StringPtr& json_schema) {
  // TODO(jimbe, mesch): This method needs a success status,
  // otherwise clients have no way to know they sent bogus data.
  new SetSchemaCall(&operation_queue_, this, json_schema);
}

void LinkImpl::Get(f1dl::VectorPtr<f1dl::StringPtr> path,
                   const std::function<void(f1dl::StringPtr)>& callback) {
  new GetCall(&operation_queue_, this, std::move(path), callback);
}

// The |src| argument identifies which client made the call to Set() or
// Update(), so that it notifies either all clients or all other clients,
// depending on whether WatchAll() or Watch() was called, respectively.
//
// When a watcher is registered, it first receives an OnChange() call with the
// current value. Thus, when a client first calls Set() and then Watch(), its
// LinkWatcher receives the value that was just Set(). This should not be
// surprising, and clients should register their watchers first before setting
// the link value.
void LinkImpl::Set(f1dl::VectorPtr<f1dl::StringPtr> path,
                   const f1dl::StringPtr& json,
                   const uint32_t src) {
  // TODO(jimbe, mesch): This method needs a success status, otherwise clients
  // have no way to know they sent bogus data.

  if (kEnableIncrementalLinks) {
    LinkChangePtr data = LinkChange::New();
    // Leave data->key null to signify a new entry
    data->op = LinkChangeOp::SET;
    data->pointer = std::move(path);
    data->json = json;
    MakeIncrementalChangeCall(std::move(data), src);
  } else {
    new SetCall(&operation_queue_, this, std::move(path), json, src);
  }
}

void LinkImpl::UpdateObject(f1dl::VectorPtr<f1dl::StringPtr> path,
                            const f1dl::StringPtr& json,
                            const uint32_t src) {
  // TODO(jimbe, mesch): This method needs a success status,
  // otherwise clients have no way to know they sent bogus data.

  if (kEnableIncrementalLinks) {
    LinkChangePtr data = LinkChange::New();
    // Leave data->key empty to signify a new entry
    data->op = LinkChangeOp::UPDATE;
    data->pointer = std::move(path);
    data->json = json;
    MakeIncrementalChangeCall(std::move(data), src);
  } else {
    new UpdateObjectCall(&operation_queue_, this, std::move(path), json, src);
  }
}

void LinkImpl::Erase(f1dl::VectorPtr<f1dl::StringPtr> path, const uint32_t src) {
  if (kEnableIncrementalLinks) {
    LinkChangePtr data = LinkChange::New();
    // Leave data->key empty to signify a new entry
    data->op = LinkChangeOp::ERASE;
    data->pointer = std::move(path);
    // Leave data->json null for ERASE.

    MakeIncrementalChangeCall(std::move(data), src);
  } else {
    new EraseCall(&operation_queue_, this, std::move(path), src);
  }
}

void LinkImpl::GetEntity(const Link::GetEntityCallback& callback) {
  new GetEntityCall(&operation_queue_, this, callback);
}

void LinkImpl::SetEntity(const f1dl::StringPtr& entity_reference,
                         const uint32_t src) {
  // SetEntity() is just a variation on Set(), so delegate to Set().
  Set(f1dl::VectorPtr<f1dl::StringPtr>::New(0),
      EntityReferenceToJson(entity_reference), src);
}

void LinkImpl::Sync(const std::function<void()>& callback) {
  new SyncCall(&operation_queue_, callback);
}

bool LinkImpl::ApplySetOp(const CrtJsonPointer& ptr, const f1dl::StringPtr& json) {
  CrtJsonDoc new_value;
  new_value.Parse(json);
  if (new_value.HasParseError()) {
    FXL_LOG(ERROR) << "LinkImpl::ApplySetOp() " << EncodeLinkPath(link_path_)
                   << " JSON parse failed error #" << new_value.GetParseError()
                   << std::endl
                   << json;
    return false;
  }

  ptr.Set(doc_, std::move(new_value));
  return true;
}

bool LinkImpl::ApplyUpdateOp(const CrtJsonPointer& ptr,
                             const f1dl::StringPtr& json) {
  CrtJsonDoc new_value;
  new_value.Parse(json);
  if (new_value.HasParseError()) {
    FXL_LOG(ERROR) << "LinkImpl::ApplyUpdateOp() " << EncodeLinkPath(link_path_)
                   << " JSON parse failed error #" << new_value.GetParseError()
                   << std::endl
                   << json;
    return false;
  }

  CrtJsonValue& current_value = ptr.Create(doc_);
  MergeObject(current_value, std::move(new_value), doc_.GetAllocator());
  return true;
}

bool LinkImpl::ApplyEraseOp(const CrtJsonPointer& ptr) {
  return ptr.Erase(doc_);
}

// Merges source into target. The values will be move()'d out of |source|.
// Returns true if the merge operation caused any changes.
bool LinkImpl::MergeObject(CrtJsonValue& target,
                           CrtJsonValue&& source,
                           CrtJsonValue::AllocatorType& allocator) {
  if (!source.IsObject()) {
    FXL_LOG(WARNING) << "LinkImpl::MergeObject() - source is not an object "
                     << JsonValueToPrettyString(source);
    return false;
  }

  if (!target.IsObject()) {
    target = std::move(source);
    return true;
  }

  bool diff = false;
  for (auto& source_itr : source.GetObject()) {
    auto target_itr = target.FindMember(source_itr.name);
    // If the value already exists and not identical, set it.
    if (target_itr == target.MemberEnd()) {
      target.AddMember(std::move(source_itr.name), std::move(source_itr.value),
                       allocator);
      diff = true;
    } else if (source_itr.value != target_itr->value) {
      // TODO(jimbe) The above comparison is O(n^2). Need to revisit the
      // detection logic.
      target_itr->value = std::move(source_itr.value);
      diff = true;
    }
  }
  return diff;
}

void LinkImpl::ValidateSchema(const char* const entry_point,
                              const CrtJsonPointer& debug_pointer,
                              const std::string& debug_json) {
  if (!schema_doc_) {
    return;
  }

  rapidjson::GenericSchemaValidator<rapidjson::SchemaDocument> validator(
      *schema_doc_);
  if (!doc_.Accept(validator)) {
    if (!validator.IsValid()) {
      rapidjson::StringBuffer sbpath;
      validator.GetInvalidSchemaPointer().StringifyUriFragment(sbpath);
      rapidjson::StringBuffer sbdoc;
      validator.GetInvalidDocumentPointer().StringifyUriFragment(sbdoc);
      rapidjson::StringBuffer sbapipath;
      debug_pointer.StringifyUriFragment(sbapipath);
      FXL_LOG(ERROR) << "Schema constraint violation in "
                     << EncodeLinkPath(link_path_) << ":" << std::endl
                     << "  Constraint " << sbpath.GetString() << "/"
                     << validator.GetInvalidSchemaKeyword() << std::endl
                     << "  Doc location: " << sbdoc.GetString() << std::endl
                     << "  API " << entry_point << std::endl
                     << "  API path " << sbapipath.GetString() << std::endl
                     << "  API json " << debug_json << std::endl;
    }
  }
}

bool LinkImpl::IsClientReadOnly(uint32_t src) {
  return create_link_info_ &&
         create_link_info_->permissions ==
             LinkPermissions::READ_ONLY_FOR_OTHERS &&
         primary_connection_ids_.count(src) == 0;
}

// To be called after:
// - API call for Set/Update/Erase. Happens at Operation execution, not
//   after PageChange event is received from the Ledger.
// - Change is received from another device in OnChange().
void LinkImpl::NotifyWatchers(const uint32_t src) {
  const f1dl::StringPtr value = JsonValueToString(doc_);
  for (auto& dst : watchers_) {
    dst->Notify(value, src);
  }
}

void LinkImpl::AddConnection(LinkConnection* const connection) {
  connections_.emplace_back(connection);
}

void LinkImpl::RemoveConnection(LinkConnection* const connection) {
  auto it =
      std::remove_if(connections_.begin(), connections_.end(),
                     [connection](const std::unique_ptr<LinkConnection>& p) {
                       return p.get() == connection;
                     });
  FXL_DCHECK(it != connections_.end());
  connections_.erase(it, connections_.end());

  // The link must be fully synced before we can call the orphaned handler
  // because the write storage call calls back onto this. Also, we must check
  // whether it's still orphaned again after Sync, because a once orphaned link
  // can acquire new connections because it can be connected to by name. This
  // requires that the orphaned handler executes synchronously.
  //
  // TODO(mesch): This is still not correct as it leaves the possibility that
  // another set operation was executed after Sync().
  if (connections_.empty() && orphaned_handler_) {
    Sync([this] {
      if (connections_.empty() && orphaned_handler_) {
        orphaned_handler_();
      }
    });
  }
}

void LinkImpl::RemoveConnection(LinkWatcherConnection* const connection) {
  auto i = std::remove_if(
      watchers_.begin(), watchers_.end(),
      [connection](const std::unique_ptr<LinkWatcherConnection>& p) {
        return p.get() == connection;
      });
  FXL_DCHECK(i != watchers_.end());
  watchers_.erase(i, watchers_.end());
}

void LinkImpl::Watch(f1dl::InterfaceHandle<LinkWatcher> watcher,
                     const uint32_t conn) {
  new WatchCall(&operation_queue_, this, std::move(watcher), conn);
}

void LinkImpl::WatchAll(f1dl::InterfaceHandle<LinkWatcher> watcher) {
  Watch(std::move(watcher), kWatchAllConnectionId);
}

LinkConnection::LinkConnection(LinkImpl* const impl,
                               const uint32_t id,
                               f1dl::InterfaceRequest<Link> link_request)
    : impl_(impl), binding_(this, std::move(link_request)), id_(id) {
  impl_->AddConnection(this);
  binding_.set_error_handler([this] { impl_->RemoveConnection(this); });
}

LinkConnection::~LinkConnection() = default;

void LinkConnection::Watch(f1dl::InterfaceHandle<LinkWatcher> watcher) {
  // This watcher stays associated with the connection it was registered
  // through. The ID is used to block notifications for updates that originate
  // at the same connection.
  impl_->Watch(std::move(watcher), id_);
}

void LinkConnection::WatchAll(f1dl::InterfaceHandle<LinkWatcher> watcher) {
  // This watcher is not associated with the connection it was registered
  // through. The connection is recorded as 0, which never identifies any
  // connection that originates an update, so no update notification is ever
  // blocked.
  impl_->WatchAll(std::move(watcher));
}

void LinkConnection::Sync(const SyncCallback& callback) {
  impl_->Sync(callback);
}

void LinkConnection::SetSchema(const f1dl::StringPtr& json_schema) {
  impl_->SetSchema(json_schema);
}

void LinkConnection::UpdateObject(f1dl::VectorPtr<f1dl::StringPtr> path,
                                  const f1dl::StringPtr& json) {
  impl_->UpdateObject(std::move(path), json, id_);
}

void LinkConnection::Set(f1dl::VectorPtr<f1dl::StringPtr> path,
                         const f1dl::StringPtr& json) {
  impl_->Set(std::move(path), json, id_);
}

void LinkConnection::Erase(f1dl::VectorPtr<f1dl::StringPtr> path) {
  impl_->Erase(std::move(path), id_);
}

void LinkConnection::GetEntity(const GetEntityCallback& callback) {
  impl_->GetEntity(std::move(callback));
}

void LinkConnection::SetEntity(const f1dl::StringPtr& entity_reference) {
  impl_->SetEntity(entity_reference, id_);
}

void LinkConnection::Get(f1dl::VectorPtr<f1dl::StringPtr> path,
                         const GetCallback& callback) {
  impl_->Get(std::move(path), callback);
}

LinkWatcherConnection::LinkWatcherConnection(LinkImpl* const impl,
                                             LinkWatcherPtr watcher,
                                             const uint32_t conn)
    : impl_(impl), watcher_(std::move(watcher)), conn_(conn) {
  watcher_.set_error_handler([this] { impl_->RemoveConnection(this); });
}

LinkWatcherConnection::~LinkWatcherConnection() = default;

void LinkWatcherConnection::Notify(const f1dl::StringPtr& value,
                                   const uint32_t src) {
  if (conn_ != src) {
    watcher_->Notify(value);
  }
}

}  // namespace modular
