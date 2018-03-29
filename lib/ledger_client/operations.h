// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The file defines Operations commonly executed on Ledger pages.

#ifndef PERIDOT_LIB_LEDGER_CLIENT_OPERATIONS_H_
#define PERIDOT_LIB_LEDGER_CLIENT_OPERATIONS_H_

#include <string>

#include <fuchsia/cpp/ledger.h>
#include "lib/async/cpp/operation.h"
#include "lib/fidl/cpp/array.h"
#include "lib/fsl/vmo/strings.h"
#include "peridot/lib/fidl/array_to_string.h"
#include "peridot/lib/fidl/json_xdr.h"
#include "peridot/lib/ledger_client/page_client.h"

namespace modular {

template <typename Data,
          typename DataPtr = std::unique_ptr<Data>,
          typename DataFilter = XdrFilterType<Data>>
class ReadDataCall : Operation<DataPtr> {
 public:
  using ResultCall = std::function<void(DataPtr)>;
  using FlowToken = typename Operation<DataPtr>::FlowToken;

  ReadDataCall(OperationContainer* const container,
               ledger::Page* const page,
               const std::string& key,
               const bool not_found_is_ok,
               DataFilter filter,
               ResultCall result_call)
      : Operation<DataPtr>("ReadDataCall",
                           container,
                           std::move(result_call),
                           key),
        page_(page),
        key_(key),
        not_found_is_ok_(not_found_is_ok),
        filter_(filter) {
    this->Ready();
  }

 private:
  void Run() override {
    FlowToken flow{this, &result_};

    page_->GetSnapshot(page_snapshot_.NewRequest(), nullptr, nullptr,
                       [this, flow](ledger::Status status) {
                         if (status != ledger::Status::OK) {
                           FXL_LOG(ERROR)
                               << this->trace_name() << " " << key_ << " "
                               << "Page.GetSnapshot() " << status;
                           return;
                         }

                         Cont(flow);
                       });
  }

  void Cont(FlowToken flow) {
    page_snapshot_->Get(
        to_array(key_),
        [this, flow](ledger::Status status, fsl::SizedVmoTransportPtr value) {
          if (status != ledger::Status::OK) {
            if (status != ledger::Status::KEY_NOT_FOUND || !not_found_is_ok_) {
              FXL_LOG(ERROR) << this->trace_name() << " " << key_ << " "
                             << "PageSnapshot.Get() " << status;
            }
            return;
          }

          if (!value) {
            FXL_LOG(ERROR) << this->trace_name() << " " << key_ << " "
                           << "PageSnapshot.Get() null vmo";
          }

          std::string value_as_string;
          if (!fsl::StringFromVmo(*value, &value_as_string)) {
            FXL_LOG(ERROR) << this->trace_name() << " " << key_
                           << " Unable to extract data.";
            return;
          }

          if (!XdrRead(value_as_string, &result_, filter_)) {
            result_.reset();
            return;
          }

          FXL_DCHECK(result_);
        });
  }

  ledger::Page* const page_;  // not owned
  const std::string key_;
  const bool not_found_is_ok_;
  DataFilter const filter_;
  ledger::PageSnapshotPtr page_snapshot_;
  DataPtr result_;

  FXL_DISALLOW_COPY_AND_ASSIGN(ReadDataCall);
};

template <typename Data,
          typename DataArray = fidl::VectorPtr<Data>,
          typename DataFilter = XdrFilterType<Data>>
class ReadAllDataCall : Operation<DataArray> {
 public:
  using ResultCall = std::function<void(DataArray)>;
  using FlowToken = typename Operation<DataArray>::FlowToken;

  ReadAllDataCall(OperationContainer* const container,
                  ledger::Page* const page,
                  std::string prefix,
                  DataFilter const filter,
                  ResultCall result_call)
      : Operation<DataArray>("ReadAllDataCall",
                             container,
                             std::move(result_call),
                             prefix),
        page_(page),
        prefix_(std::move(prefix)),
        filter_(filter) {
    data_.resize(0);
    this->Ready();
  }

 private:
  void Run() override {
    FlowToken flow{this, &data_};

    page_->GetSnapshot(page_snapshot_.NewRequest(), to_array(prefix_), nullptr,
                       [this, flow](ledger::Status status) {
                         if (status != ledger::Status::OK) {
                           FXL_LOG(ERROR) << this->trace_name() << " "
                                          << "Page.GetSnapshot() " << status;
                           return;
                         }

                         Cont1(flow);
                       });
  }

  void Cont1(FlowToken flow) {
    GetEntries(page_snapshot_.get(), &entries_,
               [this, flow](ledger::Status status) {
                 if (status != ledger::Status::OK) {
                   FXL_LOG(ERROR) << this->trace_name() << " "
                                  << "GetEntries() " << status;
                   return;
                 }

                 Cont2(flow);
               });
  }

  void Cont2(FlowToken /*flow*/) {
    for (auto& entry : entries_) {
      std::string value_as_string;
      if (!fsl::StringFromVmo(*entry.value, &value_as_string)) {
        FXL_LOG(ERROR) << this->trace_name() << " "
                       << "Unable to extract data.";
        continue;
      }

      Data data;
      if (!XdrRead(value_as_string, &data, filter_)) {
        continue;
      }

      data_.push_back(std::move(data));
    }
  }

  ledger::Page* page_;  // not owned
  ledger::PageSnapshotPtr page_snapshot_;
  const std::string prefix_;
  DataFilter const filter_;
  std::vector<ledger::Entry> entries_;
  DataArray data_;

  FXL_DISALLOW_COPY_AND_ASSIGN(ReadAllDataCall);
};

template <typename Data,
          typename DataPtr = std::unique_ptr<Data>,
          typename DataFilter = XdrFilterType<Data>>
class WriteDataCall : Operation<> {
 public:
  WriteDataCall(OperationContainer* const container,
                ledger::Page* const page,
                const std::string& key,
                DataFilter filter,
                DataPtr data,
                ResultCall result_call)
      : Operation("WriteDataCall", container, std::move(result_call), key),
        page_(page),
        key_(key),
        filter_(filter),
        data_(std::move(data)) {
    Ready();
  }

 private:
  void Run() override {
    FlowToken flow{this};

    std::string json;
    XdrWrite(&json, &data_, filter_);

    page_->Put(to_array(key_), to_array(json),
               [this, flow](ledger::Status status) {
                 if (status != ledger::Status::OK) {
                   FXL_LOG(ERROR) << this->trace_name() << " " << key_ << " "
                                  << "Page.Put() " << status;
                 }
               });
  }

  ledger::Page* const page_;  // not owned
  const std::string key_;
  DataFilter const filter_;
  DataPtr data_;

  FXL_DISALLOW_COPY_AND_ASSIGN(WriteDataCall);
};

class DumpPageSnapshotCall : Operation<std::string> {
 public:
  DumpPageSnapshotCall(OperationContainer* const container,
                       ledger::Page* const page,
                       ResultCall result_call)
      : Operation("DumpPageSnapshotCall", container, std::move(result_call)),
        page_(page) {
    Ready();
  }

 private:
  void Run() override {
    FlowToken flow{this, &dump_};

    page_->GetSnapshot(page_snapshot_.NewRequest(), nullptr, nullptr,
                       [this, flow](ledger::Status status) {
                         if (status != ledger::Status::OK) {
                           FXL_LOG(ERROR) << this->trace_name() << " "
                                          << "Page.GetSnapshot() " << status;
                           return;
                         }

                         Cont1(flow);
                       });
  }

  void Cont1(FlowToken flow) {
    GetEntries(page_snapshot_.get(), &entries_,
               [this, flow](ledger::Status status) {
                 if (status != ledger::Status::OK) {
                   FXL_LOG(ERROR) << this->trace_name() << " "
                                  << "GetEntries() " << status;
                   return;
                 }

                 Cont2(flow);
               });
  }

  void Cont2(FlowToken /*flow*/) {
    std::ostringstream stream;
    for (auto& entry : entries_) {
      stream << "key: " << to_hex_string(entry.key) << std::endl;

      std::string value_as_string;
      if (!fsl::StringFromVmo(*entry.value, &value_as_string)) {
        FXL_LOG(ERROR) << this->trace_name() << " "
                       << "Unable to extract data.";
        continue;
      }
      stream << "value: " << value_as_string << std::endl;
    }
    dump_ = stream.str();
  }

  ledger::Page* page_;  // not owned
  ledger::PageSnapshotPtr page_snapshot_;
  std::vector<ledger::Entry> entries_;
  std::string dump_;

  FXL_DISALLOW_COPY_AND_ASSIGN(DumpPageSnapshotCall);
};

}  // namespace modular

#endif  // PERIDOT_LIB_LEDGER_CLIENT_OPERATIONS_H_
