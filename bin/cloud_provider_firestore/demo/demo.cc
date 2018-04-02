// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <thread>

#include <google/firestore/v1beta1/firestore.pb.h>

#include <fuchsia/cpp/modular.h>
#include <lib/async/cpp/task.h>

#include "lib/fsl/tasks/message_loop.h"
#include "lib/fxl/command_line.h"
#include "lib/fxl/log_settings_command_line.h"
#include "peridot/bin/cloud_provider_firestore/firestore/firestore_service_impl.h"

namespace cloud_provider_firestore {
namespace {

constexpr fxl::StringView kServerIdFlag = "server-id";

void PrintUsage(const char* executable_name) {
  std::cout << "Usage: " << executable_name << " --" << kServerIdFlag
            << "=<string>" << std::endl;
}

// This is a proof-of-concept app demonstrating a single gRPC call on the
// Firestore server, to be replaced with real cloud provider.
class Demo : public modular::Lifecycle, ListenCallClient {
 public:
  explicit Demo(std::string server_id)
      : firestore_service_(std::move(server_id),
                           loop_.task_runner(),
                           MakeChannel()) {}
  ~Demo() override {}

  void Run() {
    listen_call_handler_ = firestore_service_.Listen(nullptr, this);

    async::PostDelayedTask(loop_.async(),
                          [this] { loop_.PostQuitTask(); },
                          zx::sec(20));

    loop_.Run();
  }

  // ListenCallClient:
  void OnConnected() override {
    // The watcher connection is now active.

    // Start watching for documents.
    auto request = google::firestore::v1beta1::ListenRequest();
    request.set_database(firestore_service_.GetDatabasePath());
    request.mutable_add_target()->mutable_query()->set_parent(
        firestore_service_.GetRootPath());
    request.mutable_add_target()
        ->mutable_query()
        ->mutable_structured_query()
        ->add_from()
        ->set_collection_id("top-level-collection");
    listen_call_handler_->Write(std::move(request));

    // Start creating documents.
    CreateNextDocument();
  }

  void OnResponse(
      google::firestore::v1beta1::ListenResponse response) override {
    if (response.has_document_change()) {
      FXL_LOG(INFO) << "Received notification for: "
                    << response.document_change().document().name();
    }
  }

  void OnFinished(grpc::Status status) override {
    if (!status.ok()) {
      FXL_LOG(ERROR) << "Stream closed with an error: "
                     << status.error_message()
                     << ", details: " << status.error_details();
    }
  }

 private:
  // modular::Lifecycle:
  void Terminate() override { loop_.PostQuitTask(); }

  void CreateNextDocument() {
    // Make a request that creates a new document with an "abc" field.
    auto request = google::firestore::v1beta1::CreateDocumentRequest();
    request.set_parent(firestore_service_.GetRootPath());
    request.set_collection_id("top-level-collection");
    google::firestore::v1beta1::Value forty_two;
    forty_two.set_integer_value(42);
    std::string key = "abc";
    (*(request.mutable_document()->mutable_fields()))[key] = forty_two;

    // Make the RPC and print the status.
    firestore_service_.CreateDocument(
        std::move(request), nullptr, [this](auto status, auto result) {
          if (!status.ok()) {
            FXL_LOG(ERROR) << "Failed to create the document, "
                           << "error message: " << status.error_message()
                           << ", error details: " << status.error_details();
            return;
          }
          FXL_LOG(INFO) << "Created document " << result.name();

          async::PostDelayedTask(loop_.async(),
                                 [this] { CreateNextDocument(); },
                                 zx::sec(3));
        });
  }

  std::shared_ptr<grpc::Channel> MakeChannel() {
    auto opts = grpc::SslCredentialsOptions();
    auto credentials = grpc::SslCredentials(opts);
    return grpc::CreateChannel("firestore.googleapis.com:443", credentials);
  }

  fsl::MessageLoop loop_;

  FirestoreServiceImpl firestore_service_;

  std::unique_ptr<ListenCallHandler> listen_call_handler_;

  FXL_DISALLOW_COPY_AND_ASSIGN(Demo);
};
}  // namespace

}  // namespace cloud_provider_firestore

int main(int argc, const char** argv) {
  // The trust root file is made available by the sandbox feature
  // "root-ssl-certificates"
  setenv("GRPC_DEFAULT_SSL_ROOTS_FILE_PATH", "/etc/ssl/cert.pem", 1);

  const auto command_line = fxl::CommandLineFromArgcArgv(argc, argv);
  fxl::SetLogSettingsFromCommandLine(command_line);

  std::string server_id;
  if (!command_line.GetOptionValue(
          cloud_provider_firestore::kServerIdFlag.ToString(), &server_id)) {
    cloud_provider_firestore::PrintUsage(argv[0]);
    return -1;
  }

  cloud_provider_firestore::Demo demo(std::move(server_id));
  demo.Run();

  return 0;
}
