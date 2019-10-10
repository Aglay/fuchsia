// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LEDGER_CLOUD_PROVIDER_FIRESTORE_BIN_GRPC_READ_STREAM_DRAINER_H_
#define SRC_LEDGER_CLOUD_PROVIDER_FIRESTORE_BIN_GRPC_READ_STREAM_DRAINER_H_

#include <lib/fit/function.h>

#include <functional>
#include <memory>
#include <vector>

#include "src/ledger/cloud_provider_firestore/bin/grpc/stream_controller.h"
#include "src/ledger/cloud_provider_firestore/bin/grpc/stream_reader.h"
#include "src/lib/fxl/macros.h"

#include <grpc++/grpc++.h>

namespace cloud_provider_firestore {

// Utility which can drain a read-only grpc::Stream and return the messages.
//
// |GrpcStream| template type can be any class inheriting from
// grpc::internal::AsyncReaderInterface.
template <typename GrpcStream, typename Message>
class ReadStreamDrainer {
 public:
  // Creates a new instance.
  ReadStreamDrainer(std::unique_ptr<grpc::ClientContext> context,
                    std::unique_ptr<GrpcStream> stream)
      : context_(std::move(context)),
        stream_(std::move(stream)),
        stream_controller_(stream_.get()),
        stream_reader_(stream_.get()) {}
  ~ReadStreamDrainer() = default;

  void SetOnDiscardable(fit::closure on_discardable) {
    on_discardable_ = std::move(on_discardable);
  }

  bool IsDiscardable() const { return discardable_; }

  // Reads messages from the stream until there is no more messages to read and
  // returns all the messages to the caller.
  //
  // Can be called at most once.
  void Drain(fit::function<void(grpc::Status, std::vector<Message>)> callback) {
    FXL_DCHECK(!callback_);
    callback_ = std::move(callback);
    stream_controller_.StartCall([this](bool ok) {
      if (!ok) {
        Finish();
        return;
      }

      OnConnected();
    });
  }

 private:
  void OnConnected() {
    // Configure the stream reader.
    stream_reader_.SetOnError([this] { Finish(); });
    stream_reader_.SetOnMessage([this](Message message) {
      messages_.push_back(std::move(message));
      stream_reader_.Read();
    });

    // Start reading.
    stream_reader_.Read();
  }

  void Finish() {
    stream_controller_.Finish([this](bool ok, grpc::Status status) {
      if (status.ok()) {
        callback_(status, std::move(messages_));
      } else {
        callback_(status, std::vector<Message>{});
      }

      discardable_ = true;
      if (on_discardable_) {
        on_discardable_();
      }
    });
  }

  // Context used to make the remote call.
  std::unique_ptr<grpc::ClientContext> context_;

  // gRPC stream handler.
  std::unique_ptr<GrpcStream> stream_;

  StreamController<GrpcStream> stream_controller_;
  StreamReader<GrpcStream, Message> stream_reader_;

  fit::closure on_discardable_;
  bool discardable_ = false;
  std::vector<Message> messages_;
  fit::function<void(grpc::Status, std::vector<Message>)> callback_;
  FXL_DISALLOW_COPY_AND_ASSIGN(ReadStreamDrainer);
};

}  // namespace cloud_provider_firestore

#endif  // SRC_LEDGER_CLOUD_PROVIDER_FIRESTORE_BIN_GRPC_READ_STREAM_DRAINER_H_
