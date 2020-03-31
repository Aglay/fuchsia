// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/modular/bin/sessionmgr/storage/story_storage.h"

#include <fuchsia/modular/internal/cpp/fidl.h>
#include <lib/fidl/cpp/clone.h>
#include <lib/fit/result.h>
#include <zircon/status.h>

#include "fuchsia/ledger/cpp/fidl.h"
#include "src/lib/fsl/vmo/strings.h"
#include "src/lib/fxl/strings/string_view.h"
#include "src/lib/syslog/cpp/logger.h"
#include "src/modular/bin/sessionmgr/storage/constants_and_utils.h"
#include "src/modular/bin/sessionmgr/storage/story_storage_xdr.h"
#include "src/modular/lib/fidl/clone.h"
#include "src/modular/lib/ledger_client/operations.h"

namespace modular {

namespace {

// TODO(rosswang): replace with |std::string::starts_with| after C++20
bool StartsWith(const std::string& string, const std::string& prefix) {
  return string.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

StoryStorage::StoryStorage(LedgerClient* ledger_client, fuchsia::ledger::PageId page_id)
    : PageClient("StoryStorage", ledger_client, page_id, "" /* key_prefix */),
      ledger_client_(ledger_client),
      page_id_(page_id),
      weak_ptr_factory_(this) {
  FX_DCHECK(ledger_client_ != nullptr);
}

FuturePtr<> StoryStorage::WriteModuleData(ModuleData module_data) {
  auto module_path = fidl::Clone(module_data.module_path());
  return UpdateModuleData(module_path,
                          [module_data = std::move(module_data)](ModuleDataPtr* module_data_ptr) {
                            *module_data_ptr = ModuleData::New();
                            module_data.Clone(module_data_ptr->get());
                          });
}

namespace {

struct UpdateModuleDataState {
  std::vector<std::string> module_path;
  fit::function<void(ModuleDataPtr*)> mutate_fn;
  OperationQueue sub_operations;
};

}  // namespace

FuturePtr<> StoryStorage::UpdateModuleData(const std::vector<std::string>& module_path,
                                           fit::function<void(ModuleDataPtr*)> mutate_fn) {
  auto op_state = std::make_shared<UpdateModuleDataState>();
  op_state->module_path = module_path;
  op_state->mutate_fn = std::move(mutate_fn);

  auto key = MakeModuleKey(module_path);
  auto op_body = [this, op_state, key](OperationBase* op) {
    auto did_read = Future<ModuleDataPtr>::Create("StoryStorage.UpdateModuleData.did_read");
    op_state->sub_operations.Add(std::make_unique<ReadDataCall<ModuleData>>(
        page(), key, true /* not_found_is_ok */, XdrModuleData, did_read->Completer()));

    auto did_mutate = did_read->AsyncMap([this, op_state, key](ModuleDataPtr current_module_data) {
      auto new_module_data = CloneOptional(current_module_data);
      op_state->mutate_fn(&new_module_data);

      if (!new_module_data && !current_module_data) {
        return Future<>::CreateCompleted("StoryStorage.UpdateModuleData.did_mutate");
      }

      auto module_data_copy = CloneOptional(new_module_data);
      std::string expected_value;
      XdrWrite(&expected_value, &module_data_copy, XdrModuleData);

      if (current_module_data) {
        FX_DCHECK(new_module_data) << "StoryStorage::UpdateModuleData(): mutate_fn() must not "
                                      "set to null an existing ModuleData record.";

        // We complete this Future chain when the Ledger gives us the
        // notification that |module_data| has been written. The Ledger
        // won't do that if the current value for |key| won't change, so
        // we have to short-circuit here.
        // ModuleData contains VMOs, so doing a comparison
        // *new_module_data == *current_module_data won't be true even if
        // the data in the VMOs under the hood is the same. When this
        // happens the ledger won't notify us of any change, since the data
        // was actually the same. To overcome this we compare the raw
        // strings.
        auto current_data_copy = CloneOptional(current_module_data);
        std::string current_value;
        XdrWrite(&current_value, &current_data_copy, XdrModuleData);
        if (current_value == expected_value) {
          return Future<>::CreateCompleted("StoryStorage.UpdateModuleData.did_mutate");
        }
      }

      FX_DCHECK(new_module_data->module_path() == op_state->module_path)
          << "StorageStorage::UpdateModuleData(path, ...): mutate_fn() "
             "must set "
             "ModuleData.module_path to |path|.";

      op_state->sub_operations.Add(std::make_unique<WriteDataCall<ModuleData>>(
          page(), key, XdrModuleData, std::move(module_data_copy), [] {}));

      return WaitForWrite(key, expected_value);
    });

    return did_mutate;
  };

  auto ret = Future<>::Create("StoryStorage.UpdateModuleData.ret");
  operation_queue_.Add(
      NewCallbackOperation("StoryStorage::UpdateModuleData", std::move(op_body), ret->Completer()));
  return ret;
}

FuturePtr<ModuleDataPtr> StoryStorage::ReadModuleData(const std::vector<std::string>& module_path) {
  auto key = MakeModuleKey(module_path);
  auto ret = Future<ModuleDataPtr>::Create("StoryStorage.ReadModuleData.ret");
  operation_queue_.Add(std::make_unique<ReadDataCall<ModuleData>>(
      page(), key, true /* not_found_is_ok */, XdrModuleData, ret->Completer()));
  return ret;
}

FuturePtr<std::vector<ModuleData>> StoryStorage::ReadAllModuleData() {
  auto ret = Future<std::vector<ModuleData>>::Create("StoryStorage.ReadAllModuleData.ret");
  operation_queue_.Add(std::make_unique<ReadAllDataCall<ModuleData>>(
      page(), kModuleKeyPrefix, XdrModuleData, ret->Completer()));
  return ret;
}

namespace {

constexpr char kJsonNull[] = "null";

class ReadVmoCall : public Operation<fit::result<fuchsia::mem::Buffer, fuchsia::ledger::Error>> {
 public:
  ReadVmoCall(PageClient* page_client, fidl::StringPtr key, ResultCall result_call)
      : Operation("StoryStorage::ReadVmoCall", std::move(result_call)),
        page_client_(page_client),
        key_(std::move(key)) {}

 private:
  void Run() override {
    FlowToken flow{this, &result_};

    page_snapshot_ = page_client_->NewSnapshot();
    page_snapshot_->Get(to_array(key_.value_or("")),
                        [this, flow](fuchsia::ledger::PageSnapshot_Get_Result result) {
                          if (result.is_err()) {
                            result_ = fit::error(result.err());
                          } else {
                            result_ = fit::ok(std::move(result.response().buffer));
                          }
                        });
  }

  // Input parameters.
  PageClient* const page_client_;
  const fidl::StringPtr key_;

  // Intermediate state.
  fuchsia::ledger::PageSnapshotPtr page_snapshot_;

  // Return values.
  fit::result<fuchsia::mem::Buffer, fuchsia::ledger::Error> result_;
};

// TODO(rosswang): this is a temporary migration helper
std::tuple<StoryStorage::Status, fidl::StringPtr> ToLinkValue(
    fit::result<fuchsia::mem::Buffer, fuchsia::ledger::Error> ledger_result) {
  StoryStorage::Status link_value_status = StoryStorage::Status::OK;
  fidl::StringPtr link_value;

  if (ledger_result.is_error()) {
    switch (ledger_result.error()) {
      case fuchsia::ledger::Error::KEY_NOT_FOUND:
        // Leave link_value as a null-initialized StringPtr.
        break;
      default:
        FX_LOGS(ERROR) << "PageSnapshot.Get() " << fidl::ToUnderlying(ledger_result.error());
        link_value_status = StoryStorage::Status::LEDGER_ERROR;
        break;
    }
  } else {
    std::string link_value_string;
    if (fsl::StringFromVmo(ledger_result.value(), &link_value_string)) {
      link_value = std::move(link_value_string);
    } else {
      FX_LOGS(ERROR) << "VMO could not be copied.";
      link_value_status = StoryStorage::Status::VMO_COPY_ERROR;
    }
  }

  return {link_value_status, link_value};
}

}  // namespace

FuturePtr<StoryStorage::Status, std::string> StoryStorage::GetLinkValue(const LinkPath& link_path) {
  auto key = MakeLinkKey(link_path);
  auto ret = Future<fit::result<fuchsia::mem::Buffer, fuchsia::ledger::Error>>::Create(
      "StoryStorage::GetLinkValue " + key);
  operation_queue_.Add(std::make_unique<ReadVmoCall>(this, key, ret->Completer()));

  return ret->Map(ToLinkValue)->Map([](Status status, fidl::StringPtr value) {
    return std::make_tuple(status, value ? *value : kJsonNull);
  });
}

namespace {

class WriteVmoCall : public Operation<StoryStorage::Status> {
 public:
  WriteVmoCall(PageClient* page_client, const std::string& key, fuchsia::mem::Buffer value,
               ResultCall result_call)
      : Operation("StoryStorage::WriteVmoCall", std::move(result_call)),
        page_client_(page_client),
        key_(key),
        value_(std::move(value)) {}

 private:
  void Run() override {
    FlowToken flow{this, &status_};
    status_ = StoryStorage::Status::OK;

    page_client_->page()->CreateReferenceFromBuffer(
        std::move(value_), [this, flow, weak_ptr = GetWeakPtr()](
                               fuchsia::ledger::Page_CreateReferenceFromBuffer_Result result) {
          if (weak_ptr) {
            if (result.is_response()) {
              page_client_->page()->PutReference(to_array(key_),
                                                 std::move(result.response().reference),
                                                 fuchsia::ledger::Priority::EAGER);
            } else {
              FX_LOGS(ERROR) << "StoryStorage.WriteVmoCall " << key_ << " "
                             << " Page.CreateReferenceFromBuffer() "
                             << zx_status_get_string(result.err());
              status_ = StoryStorage::Status::LEDGER_ERROR;
            }
          }
        });
  }

  PageClient* const page_client_;
  std::string key_;
  fuchsia::mem::Buffer value_;

  StoryStorage::Status status_;
};

// Returns: 1) if a mutation happened, 2) the status and 3) the new value.
class UpdateLinkCall : public Operation<bool, StoryStorage::Status, fidl::StringPtr> {
 public:
  UpdateLinkCall(
      PageClient* page_client, std::string key, fit::function<void(fidl::StringPtr*)> mutate_fn,
      fit::function<FuturePtr<>(const std::string&, const std::string&)> wait_for_write_fn,
      ResultCall done)
      : Operation("StoryStorage::UpdateLinkCall", std::move(done)),
        page_client_(page_client),
        key_(std::move(key)),
        mutate_fn_(std::move(mutate_fn)),
        wait_for_write_fn_(std::move(wait_for_write_fn)) {}

 private:
  void Run() override {
    FlowToken flow{this, &did_update_, &status_, &new_value_};

    operation_queue_.Add(std::make_unique<ReadVmoCall>(
        page_client_, key_,
        [this, flow](fit::result<fuchsia::mem::Buffer, fuchsia::ledger::Error> value) {
          fidl::StringPtr json_current_value;
          std::tie(status_, json_current_value) = ToLinkValue(std::move(value));

          if (status_ == StoryStorage::Status::OK) {
            Mutate(flow, std::move(json_current_value));
          }
        }));
  }

  void Mutate(FlowToken flow, fidl::StringPtr current_value) {
    new_value_ = current_value;
    mutate_fn_(&new_value_);

    did_update_ = true;
    if (new_value_ == current_value) {
      did_update_ = false;
      return;
    }

    fuchsia::mem::Buffer vmo;
    if (!new_value_ || fsl::VmoFromString(*new_value_, &vmo)) {
      operation_queue_.Add(std::make_unique<WriteVmoCall>(
          page_client_, key_, std::move(vmo), [this, flow](StoryStorage::Status status) {
            status_ = status;

            // If we succeeded AND we set a new value, we need to wait for
            // confirmation from the ledger.
            if (status == StoryStorage::Status::OK && new_value_.has_value()) {
              wait_for_write_fn_(key_, new_value_.value())->Then([this, flow] {
                Done(true, std::move(status_), std::move(new_value_));
              });
            }
          }));
    } else {
      FX_LOGS(ERROR) << "VMO could not be copied.";
      status_ = StoryStorage::Status::VMO_COPY_ERROR;
    }
  }

  // Input parameters.
  PageClient* const page_client_;
  const std::string key_;
  fit::function<void(fidl::StringPtr*)> mutate_fn_;
  fit::function<FuturePtr<>(const std::string&, const std::string&)> wait_for_write_fn_;

  // Operation runtime state.
  OperationQueue operation_queue_;

  // Return values.
  bool did_update_;
  StoryStorage::Status status_;
  fidl::StringPtr new_value_;
};

}  // namespace

FuturePtr<StoryStorage::Status> StoryStorage::UpdateLinkValue(
    const LinkPath& link_path, fit::function<void(fidl::StringPtr*)> mutate_fn,
    const void* context) {
  // nullptr is reserved for updates that came from other instances of
  // StoryStorage.
  FX_DCHECK(context != nullptr)
      << "StoryStorage::UpdateLinkValue(..., context) of nullptr is reserved.";

  auto key = MakeLinkKey(link_path);
  auto did_update =
      Future<bool, Status, fidl::StringPtr>::Create("StoryStorage.UpdateLinkValue.did_update");
  operation_queue_.Add(std::make_unique<UpdateLinkCall>(
      this, key, std::move(mutate_fn),
      std::bind(&StoryStorage::WaitForWrite, this, std::placeholders::_1, std::placeholders::_2),
      did_update->Completer()));

  // We can't chain this call to the parent future chain because we do
  // not want it to happen at all in the case of errors.
  return did_update->WeakMap(GetWeakPtr(), [](bool did_update, StoryStorage::Status status,
                                              fidl::StringPtr new_value) { return status; });
}

FuturePtr<> StoryStorage::Sync() {
  auto ret = Future<>::Create("StoryStorage::Sync.ret");
  operation_queue_.Add(NewCallbackOperation(
      "StoryStorage::Sync",
      [](OperationBase* op) { return Future<>::CreateCompleted("StoryStorage::Sync"); },
      ret->Completer()));
  return ret;
}

void StoryStorage::OnPageChange(const std::string& key, fuchsia::mem::BufferPtr value) {
  std::string value_string;
  if (!fsl::StringFromVmo(*value, &value_string)) {
    return;
  }

  // Find any write operations which are waiting for the notification of the
  // write being successful.
  auto pending_writes_it = pending_writes_.find({key, value_string});

  if (StartsWith(key, kModuleKeyPrefix)) {
    if (on_module_data_updated_) {
      auto module_data = ModuleData::New();
      if (!XdrRead(value_string, &module_data, XdrModuleData)) {
        FX_LOGS(ERROR) << "Unable to parse ModuleData " << key << " " << value;
        return;
      }
      on_module_data_updated_(std::move(*module_data));
    }
  }

  if (pending_writes_it != pending_writes_.end()) {
    auto local_futures = std::move(pending_writes_it->second);
    for (auto fut : local_futures) {
      // Completing this future may trigger a deletion of the story storage
      // instance, and thus leaves |this| invalid.
      fut->Complete();
    }
  }
}

void StoryStorage::OnPageDelete(const std::string& key) {
  // ModuleData and Link values are never deleted, although it is
  // theoretically possible that conflict resolution results in a key
  // disappearing. We do not currently do this.
}

void StoryStorage::OnPageConflict(Conflict* conflict) {
  // TODO(thatguy): Add basic conflict resolution. We can force a conflict for
  // link data in tests by using Page.StartTransaction() in UpdateLinkValue().
  FX_LOGS(WARNING) << "StoryStorage::OnPageConflict() for link key " << to_string(conflict->key);
}

FuturePtr<> StoryStorage::WaitForWrite(const std::string& key, const std::string& value) {
  // TODO(thatguy): It is possible that through conflict resolution, the write
  // we expect to get will never arrive.  We must have the conflict resolver
  // update |pending_writes_| with the result of conflict resolution.
  auto did_see_write = Future<>::Create("StoryStorage.WaitForWrite.did_see_write");
  pending_writes_[std::make_pair(key, value)].push_back(did_see_write);
  return did_see_write;
}

fxl::WeakPtr<StoryStorage> StoryStorage::GetWeakPtr() { return weak_ptr_factory_.GetWeakPtr(); }

}  // namespace modular
