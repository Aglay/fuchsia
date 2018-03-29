// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/user_runner/remote_invoker_impl.h"

#include <chrono>

#include "lib/fidl/cpp/array.h"
#include <fuchsia/cpp/ledger.h>
#include "peridot/lib/fidl/array_to_string.h"
#include "peridot/lib/fidl/json_xdr.h"
#include "peridot/lib/ledger_client/storage.h"

namespace modular {

namespace {
struct StoryEntry {
  std::string story_id;
  std::string timestamp;
};

void XdrStoryData(XdrContext* const xdr, StoryEntry* const data) {
  xdr->Field("story_id", &data->story_id);
  xdr->Field("timestamp", &data->timestamp);
}
}  // namespace

// Asynchronous operations of this service.

class RemoteInvokerImpl::StartOnDeviceCall : Operation<fidl::StringPtr> {
 public:
  StartOnDeviceCall(OperationContainer* const container,
                    ledger::Ledger* const ledger,
                    const fidl::StringPtr& device_id,
                    const fidl::StringPtr& story_id,
                    ResultCall result_call)
      : Operation("RemoteInvokerImpl::StartOnDeviceCall",
                  container,
                  std::move(result_call)),
        ledger_(ledger),
        device_id_(device_id),
        story_id_(story_id),
        timestamp_(std::to_string(
            std::chrono::system_clock::now().time_since_epoch().count())) {
    Ready();
  }

 private:
  void Run() override {
    FlowToken flow{this, &page_id_};

    // TODO(planders) Use Zac's function to generate page id (once it's ready)
    fidl::VectorPtr<uint8_t> page_id = to_array(device_id_);
    if (page_id->size() != 16) {
      // WARNING: HACK! Ledger page ids are 16 bytes but often we use non-16
      // byte page ids. This makes sure that the page id will be 16 bytes.
      page_id.resize(16);
    }
    ledger_->GetPage(std::move(page_id), device_page_.NewRequest(),
                     [this, flow](ledger::Status status) {
                       if (status != ledger::Status::OK) {
                         FXL_LOG(ERROR) << trace_name() << " "
                                        << "Ledger.GetPage() " << status;
                         return;
                       }

                       Cont1(flow);
                     });
  }

  void Cont1(FlowToken flow) {
    device_page_->StartTransaction([this, flow](ledger::Status status) {
      if (status != ledger::Status::OK) {
        FXL_LOG(ERROR) << trace_name() << " "
                       << "Page.StartTransaction() " << status;
        return;
      }

      Cont2(flow);
    });
  }

  void Cont2(FlowToken flow) {
    std::string json;
    StoryEntry story;
    story.story_id = story_id_;
    story.timestamp = timestamp_;
    XdrWrite(&json, &story, XdrStoryData);

    // TODO(planders) use random key
    device_page_->PutWithPriority(
        to_array(timestamp_), to_array(json), ledger::Priority::EAGER,
        [this, flow](ledger::Status status) {
          if (status != ledger::Status::OK) {
            FXL_LOG(ERROR) << trace_name() << " "
                           << "Page.PutWithPriority() " << status;
            return;
          }

          Cont3(flow);
        });
  }

  void Cont3(FlowToken flow) {
    device_page_->Commit([this, flow](ledger::Status status) {
      if (status != ledger::Status::OK) {
        FXL_LOG(ERROR) << trace_name() << " "
                       << "Page.Commit() " << status;
        return;
      }

      Cont4(flow);
    });
  }

  void Cont4(FlowToken flow) {
    device_page_->GetId([this, flow](fidl::VectorPtr<uint8_t> page_id) {
      FXL_LOG(INFO) << trace_name() << " "
                    << "Retrieved page " << to_string(page_id);
      page_id_ = to_string(page_id);
    });
  }

  ledger::Ledger* const ledger_;  // not owned
  const fidl::StringPtr device_id_;
  const fidl::StringPtr story_id_;
  const fidl::StringPtr timestamp_;
  ledger::PagePtr device_page_;
  fidl::StringPtr page_id_;

  FXL_DISALLOW_COPY_AND_ASSIGN(StartOnDeviceCall);
};

RemoteInvokerImpl::RemoteInvokerImpl(ledger::Ledger* const ledger)
    : ledger_(ledger) {}

// | RemoteService |
void RemoteInvokerImpl::StartOnDevice(const fidl::StringPtr& device_id,
                                      const fidl::StringPtr& story_id,
                                      StartOnDeviceCallback callback) {
  FXL_LOG(INFO) << "Starting rehydrate call for story " << story_id
                << " on device " << device_id;
  new StartOnDeviceCall(&operation_queue_, ledger_, device_id, story_id,
                        callback);
}

RemoteInvokerImpl::~RemoteInvokerImpl() = default;

void RemoteInvokerImpl::Connect(fidl::InterfaceRequest<RemoteInvoker> request) {
  bindings_.AddBinding(this, std::move(request));
}

}  // namespace modular
