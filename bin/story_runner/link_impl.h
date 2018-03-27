// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_STORY_RUNNER_LINK_IMPL_H_
#define PERIDOT_BIN_STORY_RUNNER_LINK_IMPL_H_

#include <vector>

#include "lib/async/cpp/operation.h"
#include "lib/fidl/cpp/bindings/binding.h"
#include "lib/fidl/cpp/bindings/interface_handle.h"
#include "lib/fidl/cpp/bindings/interface_ptr.h"
#include "lib/fidl/cpp/bindings/interface_ptr_set.h"
#include "lib/fidl/cpp/interface_request.h"
#include "lib/fxl/macros.h"
#include "lib/module/fidl/module_data.fidl.h"
#include "lib/story/fidl/create_link.fidl.h"
#include "lib/story/fidl/link.fidl.h"
#include "lib/story/fidl/link_change.fidl.h"
#include "peridot/bin/story_runner/key_generator.h"
#include "peridot/lib/ledger_client/ledger_client.h"
#include "peridot/lib/ledger_client/page_client.h"
#include "peridot/lib/ledger_client/types.h"
#include "peridot/lib/rapidjson/rapidjson.h"
#include "third_party/rapidjson/rapidjson/schema.h"

namespace modular {

// Use the CrtAllocator and not the pool allocator so that merging doesn't
// require deep copying.
using CrtJsonDoc =
    rapidjson::GenericDocument<rapidjson::UTF8<>, rapidjson::CrtAllocator>;
using CrtJsonValue = CrtJsonDoc::ValueType;
using CrtJsonPointer = rapidjson::GenericPointer<CrtJsonValue>;

class LinkConnection;
class LinkWatcherConnection;

// A Link is a mutable and observable value shared between modules.
//
// When a module requests to run more modules using
// ModuleContext::StartModuleDeprecated(), a Link instance is associated with
// each such request, i.e. a Link instance is shared between at least two
// modules. The same Link instance can be used in multiple
// StartModuleDeprecated() requests, so it can be shared between more than two
// modules. The Link instance is identified by its name in the context of the
// calling module.
//
// If a watcher is registered through one handle using the Watch() method, it
// only receives notifications for changes by requests through other handles. To
// make this possible, each Link connection is bound to a separate
// LinkConnection instance rather than to LinkImpl directly. LinkImpl owns all
// its LinkConnection instances.
//
// The value in a link may be validated against a schema. The current
// implementation is preliminary and experimental, however, in multiple ways:
//
// * The schema is not persisted. It's just imposed by some module at runtime.
//
// * It's unclear which module, or what else, should impose the schema in the
//   first place.
//
// * Schema validation is applied but failing validation is not communicated to
//   Link clients.
//
// * Because changes across devices can interact, it's possible that a set of
//   changes yields a result that is not valid according to the current schema.
//   Therefore, for now, the schema is not validated after reconciliation.
//
// This implementation of LinkImpl works by storing the history of change
// operations made by the callers. Each change operation is stored as a separate
// key/value pair, which can be reconciled by the Ledger without conflicts. The
// ordering is determined by KeyGenerator, which orders changes based on time as
// well as a random nonce that's a tie breaker in the case of changes made at
// the same time on different devices.
//
// New changes are placed on the pending_ops_ queue within the class and also
// written to the Ledger. Because the state of the Snapshot can float, the
// change operations are kept in the pending_ops_ queue until a notification is
// received from the ledger that the op has been applied to the ledger, at which
// point the change operation is removed from pending_ops_.
//
// To arrive at the latest value, the history from the ledger is merged with the
// history in pending_ops_. Duplicates are removed. Then the changes are applied
// in order. This algorithm is not "correct" due to the lack of a vector clock
// to form the partial orderings. It will be replaced eventually by a CRDT based
// one.
class LinkImpl : PageClient {
 public:
  // The |link_path| contains the series of module names (where the last element
  // is the module that created this Link) that this Link is namespaced under.
  // If |create_link_info| is null, then this is a request to connect to an
  // existing link.
  LinkImpl(LedgerClient* ledger_client,
           LedgerPageId page_id,
           LinkPathPtr link_path,
           CreateLinkInfoPtr create_link_info);

  ~LinkImpl() override;

  // Primary connections are from the module that owns the link.
  // Secondary connections are from modules that receive the link with their
  // nouns, and from the story controller. This is used to decide whether links
  // that are marked as READ_ONLY_TO_OTHERS are writable.
  enum class ConnectionType {
    Primary = 0,
    Secondary = 1,
  };

  // Creates a new LinkConnection for the given request. LinkConnection
  // instances are deleted when their connections close, and they are all
  // deleted and close their connections when LinkImpl is destroyed.
  void Connect(f1dl::InterfaceRequest<Link> request,
               ConnectionType connection_type);

  // Used by LinkConnection.
  void SetSchema(const f1dl::StringPtr& json_schema);
  void UpdateObject(f1dl::VectorPtr<f1dl::StringPtr> path,
                    const f1dl::StringPtr& json,
                    uint32_t src);
  void Set(f1dl::VectorPtr<f1dl::StringPtr> path,
           const f1dl::StringPtr& json,
           uint32_t src);
  void Get(f1dl::VectorPtr<f1dl::StringPtr> path,
           const std::function<void(f1dl::StringPtr)>& callback);
  void GetEntity(const Link::GetEntityCallback& callback);
  void SetEntity(const f1dl::StringPtr& entity_reference, const uint32_t src);
  void Erase(f1dl::VectorPtr<f1dl::StringPtr> path, uint32_t src);
  void AddConnection(LinkConnection* connection);
  void RemoveConnection(LinkConnection* connection);
  void Sync(const std::function<void()>& callback);
  void Watch(f1dl::InterfaceHandle<LinkWatcher> watcher, uint32_t conn);
  void WatchAll(f1dl::InterfaceHandle<LinkWatcher> watcher);

  // Used by LinkWatcherConnection.
  void RemoveConnection(LinkWatcherConnection* connection);

  // Returns true if the given connection is not allowed to write this Link.
  bool IsClientReadOnly(uint32_t src);

  // Used by StoryControllerImpl.
  const LinkPathPtr& link_path() const { return link_path_; }
  void set_orphaned_handler(const std::function<void()>& fn) {
    orphaned_handler_ = fn;
  }

 private:
  // |PageClient|
  void OnPageChange(const std::string& key, const std::string& value) override;

  // Applies the given |changes| to the current document. The current list of
  // pending operations is merged into the change stream. Implemented in
  // incremental_link.cc.
  void Replay(f1dl::VectorPtr<LinkChangePtr> changes);

  // Applies a single LinkChange. Implemented in incremental_link.cc.
  bool ApplyChange(LinkChange* change);

  // Implemented in incremental_link.cc.
  void MakeReloadCall(std::function<void()> done);
  void MakeIncrementalWriteCall(LinkChangePtr data, std::function<void()> done);
  void MakeIncrementalChangeCall(LinkChangePtr data, uint32_t src);

  bool ApplySetOp(const CrtJsonPointer& ptr, const f1dl::StringPtr& json);
  bool ApplyUpdateOp(const CrtJsonPointer& ptr, const f1dl::StringPtr& json);
  bool ApplyEraseOp(const CrtJsonPointer& ptr);

  static bool MergeObject(CrtJsonValue& target,
                          CrtJsonValue&& source,
                          CrtJsonValue::AllocatorType& allocator);

  void NotifyWatchers(uint32_t src);
  void ValidateSchema(const char* entry_point,
                      const CrtJsonPointer& debug_pointer,
                      const std::string& debug_json);

  // Counter for LinkConnection IDs used for sequentially assigning IDs to
  // connections. ID 0 is never used so it can be used as pseudo connection ID
  // for WatchAll() watchers. ID 1 is used as the source ID for updates from the
  // Ledger.
  uint32_t next_connection_id_{2};
  static constexpr uint32_t kWatchAllConnectionId{0};
  static constexpr uint32_t kOnChangeConnectionId{1};

  // Ids of connections that will always have write access to this link.
  // Empty by default because it's possible that none of the connected mods
  // have write access. The write access for secondary connection is determined
  // by |CreateLinkInfo|.
  std::set<uint32_t> primary_connection_ids_;

  // We can only accept connection requests once the instance is fully
  // initialized. So we queue connections on |requests_| until |ready_| is true.
  bool ready_{};
  std::vector<f1dl::InterfaceRequest<Link>> requests_;

  // Indices within |requests_| of primary connections. There is no default
  // primary connection. These values are translated to connection IDs by the
  // LinkImpl constructor as the connection IDs are generated.
  std::vector<size_t> requests_primary_indices_;

  // The value of this Link instance.
  CrtJsonDoc doc_;

  // Fidl connections to this Link instance. We need to explicitly keep track of
  // connections so we can give some watchers only notifications on changes
  // coming from *other* connections than the one the watcher was registered on.
  std::vector<std::unique_ptr<LinkConnection>> connections_;

  // Some watchers do not want notifications for changes made through the
  // connection they were registered on. Therefore, the connection they were
  // registered on is kept associated with them. The connection may still go
  // down before the watcher connection.
  //
  // Some watchers want all notifications, even from changes made through the
  // connection they were registered on. Therefore, they are not associated with
  // a connection, and the connection is recorded as nullptr. These watchers
  // obviously also may survive the connections they were registered on.
  std::vector<std::unique_ptr<LinkWatcherConnection>> watchers_;

  // The hierarchical identifier of this Link instance within its Story.
  const LinkPathPtr link_path_;

  // The attributes passed by the link creator to initialize the link.
  const CreateLinkInfoPtr create_link_info_;

  // When the Link instance loses all its Link connections, this callback is
  // invoked. It will cause the Link instance to be deleted. Remaining
  // LinkWatcher connections do not retain the Link instance, but instead can
  // watch it being deleted (through their connection error handler).
  std::function<void()> orphaned_handler_;

  // A JSON schema to be applied to the Link value.
  std::unique_ptr<rapidjson::SchemaDocument> schema_doc_;

  // Ordered key generator for incremental Link values
  KeyGenerator key_generator_;

  // Track changes that have been saved to the Ledger but not confirmed
  std::vector<LinkChangePtr> pending_ops_;

  // The latest key that's been applied to this Link. If we receive an earlier
  // key in OnChange, then replay the history.
  std::string latest_key_;

  OperationQueue operation_queue_;

  // Operations implemented here.
  class ReadLinkDataCall;
  class WriteLinkDataCall;
  class FlushWatchersCall;
  class ReadCall;
  class WriteCall;
  class GetCall;
  class SetCall;
  class SetSchemaCall;
  class UpdateObjectCall;
  class EraseCall;
  class GetEntityCall;
  class WatchCall;
  class ChangeCall;
  // Calls below are for incremental links, which can be found in
  // incremental_link.cc.
  class ReloadCall;
  class IncrementalWriteCall;
  class IncrementalChangeCall;

  FXL_DISALLOW_COPY_AND_ASSIGN(LinkImpl);
};

class LinkConnection : Link {
 public:
  ~LinkConnection() override;

  // Creates a new instance on the heap and registers it with the
  // given LinkImpl, which takes ownership. It cannot be on the stack
  // because it destroys itself when its fidl connection closes. The
  // constructor is therefore private and only accessible from here.
  static void New(LinkImpl* const impl,
                  const uint32_t id,
                  f1dl::InterfaceRequest<Link> request) {
    new LinkConnection(impl, id, std::move(request));
  }

 private:
  // Private so it cannot be created on the stack.
  LinkConnection(LinkImpl* impl,
                 uint32_t id,
                 f1dl::InterfaceRequest<Link> link_request);

  // |Link|
  void SetSchema(const f1dl::StringPtr& json_schema) override;
  void UpdateObject(f1dl::VectorPtr<f1dl::StringPtr> path,
                    const f1dl::StringPtr& json) override;
  void Set(f1dl::VectorPtr<f1dl::StringPtr> path, const f1dl::StringPtr& json) override;
  void Get(f1dl::VectorPtr<f1dl::StringPtr> path,
           const GetCallback& callback) override;
  void Erase(f1dl::VectorPtr<f1dl::StringPtr> path) override;
  void GetEntity(const GetEntityCallback& callback) override;
  void SetEntity(const f1dl::StringPtr& entity_reference) override;
  void Watch(f1dl::InterfaceHandle<LinkWatcher> watcher) override;
  void WatchAll(f1dl::InterfaceHandle<LinkWatcher> watcher) override;
  void Sync(const SyncCallback& callback) override;

  LinkImpl* const impl_;
  f1dl::Binding<Link> binding_;

  // The ID is used to identify a LinkConnection during notifications of
  // LinkWatchers about value changes, if a LinkWatcher requests to be notified
  // only of changes to the Link value made through other LinkConnections than
  // the one the LinkWatcher was registered through.
  //
  // An ID is unique within one LinkImpl instance over its whole life time. Thus
  // if a LinkConnection is closed its ID and is never reused for new
  // LinkConnection instances.
  const uint32_t id_;

  FXL_DISALLOW_COPY_AND_ASSIGN(LinkConnection);
};

class LinkWatcherConnection {
 public:
  LinkWatcherConnection(LinkImpl* impl, LinkWatcherPtr watcher, uint32_t conn);
  ~LinkWatcherConnection();

  // Notifies the LinkWatcher in this connection, unless src is the
  // LinkConnection this Watcher was registered on.
  void Notify(const f1dl::StringPtr& value, uint32_t src);

 private:
  // The LinkImpl this instance belongs to.
  LinkImpl* const impl_;

  LinkWatcherPtr watcher_;

  // The ID of the LinkConnection this LinkWatcher was registered on.
  const uint32_t conn_;

  FXL_DISALLOW_COPY_AND_ASSIGN(LinkWatcherConnection);
};

}  // namespace modular

#endif  // PERIDOT_BIN_STORY_RUNNER_LINK_IMPL_H_
