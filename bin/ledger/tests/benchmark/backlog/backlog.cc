// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/ledger/tests/benchmark/backlog/backlog.h"

#include <iostream>
//#include <cmath>

#include <fuchsia/cpp/cloud_provider.h>
#include <trace/event.h>

#include "garnet/lib/callback/waiter.h"
#include "lib/fidl/cpp/optional.h"
#include "lib/fsl/tasks/message_loop.h"
#include "lib/fsl/vmo/strings.h"
#include "lib/fxl/command_line.h"
#include "lib/fxl/files/directory.h"
#include "lib/fxl/functional/make_copyable.h"
#include "lib/fxl/logging.h"
#include "lib/fxl/strings/string_number_conversions.h"
#include "peridot/bin/ledger/testing/get_ledger.h"
#include "peridot/bin/ledger/testing/quit_on_error.h"
#include "peridot/bin/ledger/testing/run_with_tracing.h"
#include "peridot/lib/convert/convert.h"

namespace {
constexpr fxl::StringView kStoragePath = "/data/benchmark/ledger/backlog";
constexpr fxl::StringView kUniqueKeyCountFlag = "unique-key-count";
constexpr fxl::StringView kValueSizeFlag = "value-size";
constexpr fxl::StringView kCommitCountFlag = "commit-count";
constexpr fxl::StringView kRefsFlag = "refs";
constexpr fxl::StringView kServerIdFlag = "server-id";

constexpr fxl::StringView kRefsOnFlag = "on";
constexpr fxl::StringView kRefsOffFlag = "off";

constexpr size_t kKeySize = 64;
const std::string kUserDirectory = "/backlog_user";

void PrintUsage(const char* executable_name) {
  std::cout << "Usage: " << executable_name << " --" << kUniqueKeyCountFlag
            << "=<int> --" << kValueSizeFlag << "=<int> --" << kCommitCountFlag
            << "=<int> --" << kRefsFlag << "=(" << kRefsOnFlag << "|"
            << kRefsOffFlag << ") --" << kServerIdFlag << "=<string>"
            << std::endl;
}

}  // namespace

namespace test {
namespace benchmark {

BacklogBenchmark::BacklogBenchmark(
    size_t unique_key_count,
    size_t value_size,
    size_t commit_count,
    PageDataGenerator::ReferenceStrategy reference_strategy,
    std::string server_id)
    : application_context_(
          component::ApplicationContext::CreateFromStartupInfo()),
      cloud_provider_firebase_factory_(application_context_.get()),
      sync_watcher_binding_(this),
      unique_key_count_(unique_key_count),
      value_size_(value_size),
      commit_count_(commit_count),
      reference_strategy_(reference_strategy),
      server_id_(std::move(server_id)),
      writer_tmp_dir_(kStoragePath),
      reader_tmp_dir_(kStoragePath),
      done_writing_(false) {
  FXL_DCHECK(unique_key_count_ > 0);
  FXL_DCHECK(value_size_ > 0);
  FXL_DCHECK(commit_count_ > 0);
  cloud_provider_firebase_factory_.Init();
}

void BacklogBenchmark::SyncStateChanged(ledger::SyncState download,
                                        ledger::SyncState upload,
                                        SyncStateChangedCallback callback) {
  if (on_sync_state_changed_) {
    on_sync_state_changed_(download, upload);
  }
  callback();
}

void BacklogBenchmark::Run() {
  // Name of the storage directory currently identifies the user. Ensure the
  // most nested directory has the same name to make the ledgers sync.
  std::string writer_path = writer_tmp_dir_.path() + kUserDirectory;
  FXL_DCHECK(files::CreateDirectory(writer_path));

  cloud_provider::CloudProviderPtr cloud_provider_writer;
  cloud_provider_firebase_factory_.MakeCloudProvider(
      server_id_, "backlog", cloud_provider_writer.NewRequest());
  ledger::Status status = test::GetLedger(
      fsl::MessageLoop::GetCurrent(), application_context_.get(),
      &writer_controller_, std::move(cloud_provider_writer), "backlog",
      writer_path, &writer_);
  QuitOnError(status, "Get writer ledger");

  status =
      test::GetPageEnsureInitialized(fsl::MessageLoop::GetCurrent(), &writer_,
                                     nullptr, &writer_page_, &page_id_);
  QuitOnError(status, "Writer page initialization");

  WaitForWriterUpload();
  TRACE_ASYNC_BEGIN("benchmark", "populate and upload", 0);
  TRACE_ASYNC_BEGIN("benchmark", "populate", 0);
  Populate();
}

void BacklogBenchmark::Populate() {
  int transaction_size = static_cast<int>(
      ceil(static_cast<double>(unique_key_count_) / commit_count_));
  int key_count = std::max(unique_key_count_, commit_count_);
  FXL_LOG(INFO) << "Transaction size: " << transaction_size
                << ", key count: " << key_count << ".";
  auto keys = generator_.MakeKeys(key_count, kKeySize, unique_key_count_);
  page_data_generator_.Populate(
      &writer_page_, std::move(keys), value_size_, transaction_size,
      reference_strategy_, ledger::Priority::EAGER,
      [this](ledger::Status status) {
        if (status != ledger::Status::OK) {
          benchmark::QuitOnError(status, "PageGenerator::Populate");
          return;
        }
        done_writing_ = true;
        TRACE_ASYNC_END("benchmark", "populate", 0);
      });
}

void BacklogBenchmark::WaitForWriterUpload() {
  on_sync_state_changed_ = [this](ledger::SyncState download,
                                  ledger::SyncState upload) {
    if ((upload == ledger::SyncState::IDLE) && done_writing_) {
      on_sync_state_changed_ = nullptr;
      TRACE_ASYNC_END("benchmark", "populate and upload", 0);
      // Stop watching sync state for this page.
      sync_watcher_binding_.Unbind();
      ConnectReader();
      return;
    }
  };
  writer_page_->SetSyncStateWatcher(
      sync_watcher_binding_.NewBinding(),
      benchmark::QuitOnErrorCallback("Page::SetSyncStateWatcher"));
}

void BacklogBenchmark::ConnectReader() {
  std::string reader_path = reader_tmp_dir_.path() + kUserDirectory;
  FXL_DCHECK(files::CreateDirectory(reader_path));

  cloud_provider::CloudProviderPtr cloud_provider_reader;
  cloud_provider_firebase_factory_.MakeCloudProvider(
      server_id_, "backlog", cloud_provider_reader.NewRequest());
  ledger::Status status = test::GetLedger(
      fsl::MessageLoop::GetCurrent(), application_context_.get(),
      &reader_controller_, std::move(cloud_provider_reader), "backlog",
      reader_path, &reader_);
  QuitOnError(status, "ConnectReader");

  TRACE_ASYNC_BEGIN("benchmark", "download", 0);
  TRACE_ASYNC_BEGIN("benchmark", "get page", 0);
  reader_->GetPage(fidl::MakeOptional(page_id_), reader_page_.NewRequest(),
                   [this](ledger::Status status) {
                     if (benchmark::QuitOnError(status, "GetPage")) {
                       return;
                     }
                     TRACE_ASYNC_END("benchmark", "get page", 0);
                     WaitForReaderDownload();
                   });
}

void BacklogBenchmark::WaitForReaderDownload() {
  previous_state_ = ledger::SyncState::IDLE;
  on_sync_state_changed_ = [this](ledger::SyncState download,
                                  ledger::SyncState upload) {
    if (download == ledger::SyncState::IDLE &&
        previous_state_ != ledger::SyncState::IDLE) {
      on_sync_state_changed_ = nullptr;
      TRACE_ASYNC_END("benchmark", "download", 0);
      GetReaderSnapshot();
      return;
    }
    // Workaround to skip first (IDLE, IDLE) state before the download starts,
    // see LE-369
    previous_state_ = download;
  };
  reader_page_->SetSyncStateWatcher(
      sync_watcher_binding_.NewBinding(),
      benchmark::QuitOnErrorCallback("Page::SetSyncStateWatcher"));
}

void BacklogBenchmark::GetReaderSnapshot() {
  reader_page_->GetSnapshot(reader_snapshot_.NewRequest(), nullptr, nullptr,
                            benchmark::QuitOnErrorCallback("GetSnapshot"));
  TRACE_ASYNC_BEGIN("benchmark", "get all entries", 0);
  GetEntriesStep(nullptr, unique_key_count_);
}

void BacklogBenchmark::CheckStatusAndGetMore(
    ledger::Status status,
    size_t entries_left,
    fidl::VectorPtr<uint8_t> next_token) {
  if ((status != ledger::Status::OK) &&
      (status != ledger::Status::PARTIAL_RESULT)) {
    QuitOnError(status, "PageSnapshot::GetEntries");
  }
  if (status == ledger::Status::OK) {
    TRACE_ASYNC_END("benchmark", "get all entries", 0);
    FXL_DCHECK(entries_left == 0);
    FXL_DCHECK(!next_token);
    ShutDown();
    return;
  }
  FXL_DCHECK(next_token);
  GetEntriesStep(std::move(next_token), entries_left);
}

void BacklogBenchmark::GetEntriesStep(fidl::VectorPtr<uint8_t> token,
                                      size_t entries_left) {
  FXL_DCHECK(entries_left > 0);
  TRACE_ASYNC_BEGIN("benchmark", "get entries partial", entries_left);
  if (reference_strategy_ == PageDataGenerator::ReferenceStrategy::INLINE) {
    reader_snapshot_->GetEntriesInline(
        nullptr, std::move(token),
        fxl::MakeCopyable([this, entries_left](ledger::Status status,
                                               auto entries,
                                               auto next_token) mutable {
          TRACE_ASYNC_END("benchmark", "get entries partial", entries_left);
          CheckStatusAndGetMore(status, entries_left - entries->size(),
                                std::move(next_token));
        }));
  } else {
    reader_snapshot_->GetEntries(
        nullptr, std::move(token),
        fxl::MakeCopyable([this, entries_left](ledger::Status status,
                                               auto entries,
                                               auto next_token) mutable {
          TRACE_ASYNC_END("benchmark", "get entries partial", entries_left);
          CheckStatusAndGetMore(status, entries_left - entries->size(),
                                std::move(next_token));
        }));
  }
}

void BacklogBenchmark::ShutDown() {
  writer_controller_->Kill();
  writer_controller_.WaitForResponseUntil(zx::deadline_after(zx::sec(5)));
  reader_controller_->Kill();
  reader_controller_.WaitForResponseUntil(zx::deadline_after(zx::sec(5)));

  fsl::MessageLoop::GetCurrent()->PostQuitTask();
}

}  // namespace benchmark
}  // namespace test

int main(int argc, const char** argv) {
  fxl::CommandLine command_line = fxl::CommandLineFromArgcArgv(argc, argv);

  std::string unique_key_count_str;
  size_t unique_key_count;
  std::string value_size_str;
  size_t value_size;
  std::string commit_count_str;
  size_t commit_count;
  std::string reference_strategy_str;
  std::string server_id;
  if (!command_line.GetOptionValue(kUniqueKeyCountFlag.ToString(),
                                   &unique_key_count_str) ||
      !fxl::StringToNumberWithError(unique_key_count_str, &unique_key_count) ||
      unique_key_count <= 0 ||
      !command_line.GetOptionValue(kValueSizeFlag.ToString(),
                                   &value_size_str) ||
      !fxl::StringToNumberWithError(value_size_str, &value_size) ||
      value_size <= 0 ||
      !command_line.GetOptionValue(kCommitCountFlag.ToString(),
                                   &commit_count_str) ||
      !fxl::StringToNumberWithError(commit_count_str, &commit_count) ||
      commit_count <= 0 ||
      !command_line.GetOptionValue(kRefsFlag.ToString(),
                                   &reference_strategy_str) ||
      !command_line.GetOptionValue(kServerIdFlag.ToString(), &server_id)) {
    PrintUsage(argv[0]);
    return -1;
  }

  test::benchmark::PageDataGenerator::ReferenceStrategy reference_strategy;
  if (reference_strategy_str == kRefsOnFlag) {
    reference_strategy =
        test::benchmark::PageDataGenerator::ReferenceStrategy::REFERENCE;
  } else if (reference_strategy_str == kRefsOffFlag) {
    reference_strategy =
        test::benchmark::PageDataGenerator::ReferenceStrategy::INLINE;
  } else {
    std::cerr << "Unknown option " << reference_strategy_str << " for "
              << kRefsFlag.ToString() << std::endl;
    PrintUsage(argv[0]);
    return -1;
  }

  fsl::MessageLoop loop;
  test::benchmark::BacklogBenchmark app(unique_key_count, value_size,
                                        commit_count, reference_strategy,
                                        server_id);
  return test::benchmark::RunWithTracing(&loop, [&app] { app.Run(); });
}
