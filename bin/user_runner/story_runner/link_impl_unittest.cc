// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/user_runner/story_runner/link_impl.h"
#include <fuchsia/modular/cpp/fidl.h>
#include "gtest/gtest.h"
#include "lib/async/cpp/operation.h"
#include "lib/fidl/cpp/array.h"
#include "peridot/lib/fidl/array_to_string.h"
#include "peridot/lib/fidl/clone.h"
#include "peridot/lib/fidl/json_xdr.h"
#include "peridot/lib/ledger_client/ledger_client.h"
#include "peridot/lib/ledger_client/page_client.h"
#include "peridot/lib/ledger_client/page_id.h"
#include "peridot/lib/ledger_client/storage.h"
#include "peridot/lib/rapidjson/rapidjson.h"
#include "peridot/lib/testing/test_with_ledger.h"
#include "peridot/public/lib/entity/cpp/json.h"

namespace fuchsia {
namespace modular {
namespace internal {
class LinkChange;
}  // namespace internal

// Defined in incremental_link.cc.
extern const XdrFilterType<internal::LinkChange> XdrLinkChange[];

namespace {
const char kInitialLinkValue[] = "{}";
}  // namespace

namespace {

LinkPath GetTestLinkPath() {
  LinkPath link_path;
  link_path.module_path.push_back("root");
  link_path.module_path.push_back("photos");
  link_path.link_name = "theLinkName";
  return link_path;
}

// TODO(mesch): Duplicated from ledger_client.cc.
bool HasPrefix(const std::string& value, const std::string& prefix) {
  if (value.size() < prefix.size()) {
    return false;
  }

  for (size_t i = 0; i < prefix.size(); ++i) {
    if (value[i] != prefix[i]) {
      return false;
    }
  }

  return true;
}

class PageClientPeer : fuchsia::modular::PageClient {
 public:
  PageClientPeer(LedgerClient* const ledger_client,
                 LedgerPageId page_id,
                 std::string expected_prefix)
      : PageClient("PageClientPeer", ledger_client, std::move(page_id)),
        expected_prefix_(std::move(expected_prefix)) {}

  void OnPageChange(const std::string& key, const std::string& value) {
    EXPECT_TRUE(HasPrefix(key, expected_prefix_))
        << " key=" << key << " expected_prefix=" << expected_prefix_;
    changes.push_back(std::make_pair(key, value));

    EXPECT_TRUE(XdrRead(value, &last_change, XdrLinkChange))
        << key << " " << value;
    FXL_LOG(INFO) << "PageChange " << key << " = " << value;
  };

  std::vector<std::pair<std::string, std::string>> changes;
  modular::internal ::LinkChangePtr last_change;

 private:
  std::string expected_prefix_;
};

class LinkImplTestBase : public testing::TestWithLedger,
                         fuchsia::modular::LinkWatcher {
 public:
  LinkImplTestBase() : watcher_binding_(this) {}

  virtual CreateLinkInfoPtr GetCreateLinkInfo() = 0;

  void SetUp() override {
    TestWithLedger::SetUp();

    OperationBase::set_observer([this](const char* const operation_name) {
      ++operations_[operation_name];
    });

    auto page_id = MakePageId("0123456789123456");
    auto link_path = GetTestLinkPath();

    auto create_link_info = GetCreateLinkInfo();

    link_impl_ =
        std::make_unique<LinkImpl>(ledger_client(), CloneStruct(page_id),
                                   link_path, std::move(create_link_info));

    link_impl_->Connect(link_.NewRequest());

    ledger_client_peer_ = ledger_client()->GetLedgerClientPeer();
    page_client_peer_ = std::make_unique<PageClientPeer>(
        ledger_client_peer_.get(), CloneStruct(page_id),
        MakeLinkKey(link_path));
  }

  void TearDown() override {
    if (watcher_binding_.is_bound()) {
      watcher_binding_.Unbind();
    }

    link_impl_.reset();
    link_.Unbind();

    page_client_peer_.reset();
    ledger_client_peer_.reset();

    OperationBase::set_observer(nullptr);

    TestWithLedger::TearDown();
  }

  int ledger_change_count() const { return page_client_peer_->changes.size(); }

  modular::internal ::LinkChangePtr& last_change() {
    return page_client_peer_->last_change;
  }

  void ExpectOneCall(const std::string& operation_name) {
    EXPECT_EQ(1u, operations_.count(operation_name))
        << operation_name << " was not called.";
    EXPECT_EQ(1, operations_[operation_name]) << operation_name;
    operations_.erase(operation_name);
  }

  void ExpectAtLeastCalls(const std::string& operation_name, int n) {
    EXPECT_EQ(1u, operations_.count(operation_name))
        << operation_name << " was not called.";
    EXPECT_LE(n, operations_[operation_name]) << operation_name;
    operations_.erase(operation_name);
  }

  void ExpectCalls(const std::string& operation_name, int n) {
    EXPECT_EQ(1u, operations_.count(operation_name))
        << operation_name << " was not called.";
    EXPECT_EQ(n, operations_[operation_name]) << operation_name;
    operations_.erase(operation_name);
  }

  void ExpectNoOtherCalls() {
    EXPECT_TRUE(operations_.empty());
    for (const auto& c : operations_) {
      FXL_LOG(INFO) << "    Unexpected call: " << c.first;
    }
  }

  void ClearCalls() { operations_.clear(); }

  void Notify(fidl::StringPtr json) override {
    step_++;
    last_json_notify_ = json;
    continue_();
  };

  std::unique_ptr<LinkImpl> link_impl_;
  LinkPtr link_;

  std::unique_ptr<LedgerClient> ledger_client_peer_;
  std::unique_ptr<PageClientPeer> page_client_peer_;

  fidl::Binding<fuchsia::modular::LinkWatcher> watcher_binding_;
  int step_{};
  std::string last_json_notify_;
  std::function<void()> continue_;

  std::map<std::string, int> operations_;
};

class LinkImplTest : public LinkImplTestBase {
 public:
  LinkImplTest() = default;
  ~LinkImplTest() = default;

  CreateLinkInfoPtr GetCreateLinkInfo() override {
    auto create_link_info = CreateLinkInfo::New();
    create_link_info->initial_data = kInitialLinkValue;
    // |create_link_info->allowed_types| is already null.
    return create_link_info;
  }
};

TEST_F(LinkImplTest, Constructor) {
  continue_ = [this] { EXPECT_LE(step_, 1); };

  link_->WatchAll(watcher_binding_.NewBinding());

  bool synced{};
  link_->Sync([&synced] { synced = true; });

  EXPECT_TRUE(RunLoopUntilWithTimeout([&synced] { return synced; }));

  EXPECT_EQ(1, ledger_change_count());

  EXPECT_EQ(kInitialLinkValue, last_json_notify_);
  ExpectOneCall("LinkImpl::ReloadCall");
  ExpectOneCall("ReadAllDataCall");
  // All numbers for |IncrementalChangeCall| are "at least" because PageClient
  // will make a callback once per write, effectively doubling the number of
  // calls. However, |LinkImpl::OnPageChange| puts those requests on an
  // OperationQueue, so each request may or may not have run by the time Sync()
  // returns.
  ExpectAtLeastCalls("LinkImpl::IncrementalChangeCall", ledger_change_count());
  ExpectCalls("LinkImpl::IncrementalWriteCall", ledger_change_count());
  ExpectCalls("WriteDataCall", ledger_change_count());
  ExpectOneCall("LinkImpl::WatchCall");
  ExpectOneCall("SyncCall");
  ExpectNoOtherCalls();
}

TEST_F(LinkImplTest, Set) {
  continue_ = [this] { EXPECT_LE(step_, 2); };

  link_->WatchAll(watcher_binding_.NewBinding());
  link_->Set(nullptr, "{ \"value\": 7 }");

  bool synced{};
  link_->Sync([&synced] { synced = true; });

  EXPECT_TRUE(RunLoopUntilWithTimeout([&synced] { return synced; }));

  EXPECT_EQ(2, ledger_change_count());

  // Calls from constructor and setup.
  ExpectOneCall("LinkImpl::ReloadCall");
  ExpectOneCall("ReadAllDataCall");
  ExpectOneCall("LinkImpl::WatchCall");
  // Calls from Set().
  ExpectAtLeastCalls("LinkImpl::IncrementalChangeCall", ledger_change_count());
  ExpectCalls("LinkImpl::IncrementalWriteCall", ledger_change_count());
  ExpectCalls("WriteDataCall", ledger_change_count());
  ExpectOneCall("SyncCall");
  ExpectNoOtherCalls();
  EXPECT_EQ("{\"value\":7}", last_json_notify_);
}

TEST_F(LinkImplTest, Update) {
  continue_ = [this] { EXPECT_LE(step_, 3); };

  link_->WatchAll(watcher_binding_.NewBinding());

  link_->Set(nullptr, "{ \"value\": 8 }");
  link_->UpdateObject(nullptr, "{ \"value\": 50 }");

  bool synced{};
  link_->Sync([&synced] { synced = true; });

  EXPECT_TRUE(RunLoopUntilWithTimeout([&synced] { return synced; }));

  EXPECT_EQ(3, ledger_change_count());

  ExpectAtLeastCalls("LinkImpl::IncrementalChangeCall", ledger_change_count());
  ExpectCalls("LinkImpl::IncrementalWriteCall", ledger_change_count());
  ExpectCalls("WriteDataCall", ledger_change_count());

  EXPECT_EQ("{\"value\":50}", last_change()->json);
  EXPECT_EQ("{\"value\":50}", last_json_notify_);
}

TEST_F(LinkImplTest, UpdateNewKey) {
  continue_ = [this] { EXPECT_LE(step_, 3); };

  link_->WatchAll(watcher_binding_.NewBinding());

  link_->Set(nullptr, "{ \"value\": 9 }");
  link_->UpdateObject(nullptr, "{ \"century\": 100 }");

  bool synced{};
  link_->Sync([&synced] { synced = true; });

  EXPECT_TRUE(RunLoopUntilWithTimeout([&synced] { return synced; }));

  EXPECT_EQ(3, ledger_change_count());

  ExpectAtLeastCalls("LinkImpl::IncrementalChangeCall", ledger_change_count());
  ExpectCalls("LinkImpl::IncrementalWriteCall", ledger_change_count());
  ExpectCalls("WriteDataCall", ledger_change_count());

  EXPECT_EQ("{\"century\":100}", last_change()->json);
  EXPECT_EQ("{\"value\":9,\"century\":100}", last_json_notify_);
}

TEST_F(LinkImplTest, Erase) {
  continue_ = [this] { EXPECT_LE(step_, 3); };

  link_->WatchAll(watcher_binding_.NewBinding());

  link_->Set(nullptr, "{ \"value\": 4 }");

  fidl::VectorPtr<fidl::StringPtr> segments;
  segments.push_back("value");
  link_->Erase(std::move(segments));

  bool synced{};
  link_->Sync([&synced] { synced = true; });

  EXPECT_TRUE(RunLoopUntilWithTimeout([&synced] { return synced; }));

  EXPECT_EQ(3, ledger_change_count());

  ExpectAtLeastCalls("LinkImpl::IncrementalChangeCall", ledger_change_count());
  ExpectCalls("LinkImpl::IncrementalWriteCall", ledger_change_count());
  ExpectCalls("WriteDataCall", ledger_change_count());

  EXPECT_TRUE(last_change()->json.is_null());
  EXPECT_EQ("{}", last_json_notify_);
}

TEST_F(LinkImplTest, SetEntity) {
  continue_ = [this] { EXPECT_LE(step_, 4); };

  const char entity_ref[] = "entertaining-entity";
  const std::string entity_ref_json = EntityReferenceToJson(entity_ref);

  link_->WatchAll(watcher_binding_.NewBinding());
  link_->SetEntity(entity_ref);

  bool synced{};
  link_->Sync([&synced] { synced = true; });

  EXPECT_TRUE(RunLoopUntilWithTimeout([&synced] { return synced; }));

  EXPECT_EQ(2, ledger_change_count());

  ExpectAtLeastCalls("LinkImpl::IncrementalChangeCall", ledger_change_count());
  ExpectCalls("LinkImpl::IncrementalWriteCall", ledger_change_count());
  ExpectCalls("WriteDataCall", ledger_change_count());

  // SetEntity() delegates to Set(), which was tested above, so don't
  // repeat those tests here.
  EXPECT_EQ(entity_ref_json, last_json_notify_);

  bool done{};
  link_->GetEntity([entity_ref, &done](const fidl::StringPtr value) {
    EXPECT_EQ(entity_ref, value);
    done = true;
  });
  EXPECT_TRUE(RunLoopUntilWithTimeout([&done] { return done; }));
}

class LinkImplNullInitTest : public LinkImplTestBase {
 public:
  LinkImplNullInitTest() = default;
  ~LinkImplNullInitTest() = default;

  CreateLinkInfoPtr GetCreateLinkInfo() override { return CreateLinkInfoPtr(); }
};

TEST_F(LinkImplNullInitTest, Set) {
  // Even though we only write one value, we get two notifications, one
  // for the initial value of null and one for the Set() call below.
  continue_ = [this] { EXPECT_LE(step_, 2); };

  link_->WatchAll(watcher_binding_.NewBinding());

  link_->Set(nullptr, "\"from_link\"");

  EXPECT_TRUE(RunLoopUntilWithTimeout([this] {
    return "\"from_link\"" == last_json_notify_ && ledger_change_count() == 1;
  }));
}

// TODO(jimbe) Still many tests to be written, including:
//
// * Specific behavior of LinkWatcher notification (Watch() not called for own
//   changes, Watch() and WatchAll() only called for changes that really occur,
//   and only once.

}  // namespace
}  // namespace modular
}  // namespace fuchsia
