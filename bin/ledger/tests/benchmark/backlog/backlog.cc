// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/ledger/tests/benchmark/backlog/backlog.h"

#include <iostream>

#include <fuchsia/ledger/cloud/cpp/fidl.h>
#include <lib/callback/waiter.h>
#include <lib/fidl/cpp/optional.h>
#include <lib/fit/function.h>
#include <lib/fsl/vmo/strings.h>
#include <lib/fxl/command_line.h>
#include <lib/fxl/files/directory.h>
#include <lib/fxl/files/file.h>
#include <lib/fxl/logging.h>
#include <lib/fxl/random/uuid.h>
#include <lib/fxl/strings/string_number_conversions.h>
#include <trace/event.h>
#include "lib/fidl/cpp/clone.h"

#include "peridot/bin/ledger/filesystem/get_directory_content_size.h"
#include "peridot/bin/ledger/testing/get_ledger.h"
#include "peridot/bin/ledger/testing/get_page_ensure_initialized.h"
#include "peridot/bin/ledger/testing/quit_on_error.h"
#include "peridot/bin/ledger/testing/run_with_tracing.h"
#include "peridot/lib/convert/convert.h"

namespace {
constexpr fxl::StringView kBinaryPath =
    "fuchsia-pkg://fuchsia.com/ledger_benchmarks#meta/backlog.cmx";
constexpr fxl::StringView kStoragePath = "/data/benchmark/ledger/backlog";
constexpr fxl::StringView kUniqueKeyCountFlag = "unique-key-count";
constexpr fxl::StringView kKeySizeFlag = "key-size";
constexpr fxl::StringView kValueSizeFlag = "value-size";
constexpr fxl::StringView kCommitCountFlag = "commit-count";
constexpr fxl::StringView kRefsFlag = "refs";
constexpr fxl::StringView kRefsOnFlag = "on";
constexpr fxl::StringView kRefsOffFlag = "off";

const std::string kUserDirectory = "/backlog_user";

void PrintUsage() {
  std::cout << "Usage: trace record "
            << kBinaryPath
            // Comment to make clang format not break formatting.
            << " --" << kUniqueKeyCountFlag << "=<int>"
            << " --" << kKeySizeFlag << "=<int>"
            << " --" << kValueSizeFlag << "=<int>"
            << " --" << kCommitCountFlag << "=<int>"
            << " --" << kRefsFlag << "=(" << kRefsOnFlag << "|" << kRefsOffFlag
            << ")" << ledger::GetSyncParamsUsage() << std::endl;
}

}  // namespace

namespace ledger {

BacklogBenchmark::BacklogBenchmark(
    async::Loop* loop, size_t unique_key_count, size_t key_size,
    size_t value_size, size_t commit_count,
    PageDataGenerator::ReferenceStrategy reference_strategy,
    SyncParams sync_params)
    : loop_(loop),
      startup_context_(component::StartupContext::CreateFromStartupInfo()),
      cloud_provider_factory_(startup_context_.get(),
                              std::move(sync_params.api_key),
                              std::move(sync_params.credentials)),
      sync_watcher_binding_(this),
      unique_key_count_(unique_key_count),
      key_size_(key_size),
      value_size_(value_size),
      commit_count_(commit_count),
      reference_strategy_(reference_strategy),
      user_id_("backlog_" + fxl::GenerateUUID()),
      writer_tmp_dir_(kStoragePath),
      reader_tmp_dir_(kStoragePath) {
  FXL_DCHECK(loop_);
  FXL_DCHECK(unique_key_count_ > 0);
  FXL_DCHECK(key_size_ > 0);
  FXL_DCHECK(value_size_ > 0);
  FXL_DCHECK(commit_count_ > 0);
  cloud_provider_factory_.Init();
}

void BacklogBenchmark::SyncStateChanged(SyncState download, SyncState upload,
                                        SyncStateChangedCallback callback) {
  if (on_sync_state_changed_) {
    on_sync_state_changed_(download, upload);
  }
  callback();
}

void BacklogBenchmark::Run() { ConnectWriter(); }

void BacklogBenchmark::ConnectWriter() {
  // Name of the storage directory currently identifies the user. Ensure the
  // most nested directory has the same name to make the ledgers sync.
  std::string writer_path = writer_tmp_dir_.path() + kUserDirectory;
  bool ret = files::CreateDirectory(writer_path);
  FXL_DCHECK(ret);

  GetLedger(startup_context_.get(), writer_controller_.NewRequest(), nullptr,
            "backlog", DetachedPath(std::move(writer_path)),
            []() { FXL_LOG(INFO) << "Writer closed."; },
            [this](Status status, LedgerPtr writer) {
              if (QuitOnError(QuitLoopClosure(), status, "Get writer ledger")) {
                return;
              }
              writer_ = std::move(writer);

              GetPageEnsureInitialized(
                  &writer_, nullptr,
                  []() { FXL_LOG(INFO) << "Writer page closed."; },
                  [this](Status status, PagePtr writer_page, PageId page_id) {
                    if (QuitOnError(QuitLoopClosure(), status,
                                    "Writer page initialization")) {
                      return;
                    }

                    writer_page_ = std::move(writer_page);
                    page_id_ = page_id;

                    TRACE_ASYNC_BEGIN("benchmark", "populate", 0);
                    Populate();
                  });
            });
}

void BacklogBenchmark::Populate() {
  int transaction_size = static_cast<int>(
      ceil(static_cast<double>(unique_key_count_) / commit_count_));
  int key_count = std::max(unique_key_count_, commit_count_);
  FXL_LOG(INFO) << "Transaction size: " << transaction_size
                << ", key count: " << key_count << ".";
  auto keys = generator_.MakeKeys(key_count, key_size_, unique_key_count_);
  page_data_generator_.Populate(
      &writer_page_, std::move(keys), value_size_, transaction_size,
      reference_strategy_, Priority::EAGER, [this](Status status) {
        if (status != Status::OK) {
          if (QuitOnError(QuitLoopClosure(), status,
                          "PageGenerator::Populate")) {
            return;
          }
          return;
        }
        TRACE_ASYNC_END("benchmark", "populate", 0);
        DisconnectAndRecordWriter();
      });
}

void BacklogBenchmark::DisconnectAndRecordWriter() {
  KillLedgerProcess(&writer_controller_);
  RecordDirectorySize("writer_directory_size", writer_tmp_dir_.path());
  ConnectUploader();
}

void BacklogBenchmark::ConnectUploader() {
  // Name of the storage directory currently identifies the user. Ensure the
  // most nested directory has the same name to make the ledgers sync.
  std::string uploader_path = writer_tmp_dir_.path() + kUserDirectory;

  cloud_provider::CloudProviderPtr cloud_provider_uploader;
  cloud_provider_factory_.MakeCloudProviderWithGivenUserId(
      user_id_, cloud_provider_uploader.NewRequest());
  GetLedger(
      startup_context_.get(), uploader_controller_.NewRequest(),
      std::move(cloud_provider_uploader), "backlog",
      DetachedPath(std::move(uploader_path)), QuitLoopClosure(),
      [this](Status status, LedgerPtr uploader) {
        if (QuitOnError(QuitLoopClosure(), status, "Get uploader ledger")) {
          return;
        }
        uploader_ = std::move(uploader);

        TRACE_ASYNC_BEGIN("benchmark", "get_uploader_page", 0);
        TRACE_ASYNC_BEGIN("benchmark", "upload", 0);
        uploader_->GetPage(
            fidl::MakeOptional(fidl::Clone(page_id_)),
            uploader_page_.NewRequest(), [this](Status status) {
              if (QuitOnError(QuitLoopClosure(), status, "GetPage")) {
                return;
              }
              TRACE_ASYNC_END("benchmark", "get_uploader_page", 0);
              WaitForUploaderUpload();
            });
      });
}

void BacklogBenchmark::WaitForUploaderUpload() {
  on_sync_state_changed_ = [this](SyncState download, SyncState upload) {
    if (upload == SyncState::IDLE) {
      on_sync_state_changed_ = nullptr;
      TRACE_ASYNC_END("benchmark", "upload", 0);
      // Stop watching sync state for this page.
      sync_watcher_binding_.Unbind();
      ConnectReader();
      return;
    }
  };
  uploader_page_->SetSyncStateWatcher(
      sync_watcher_binding_.NewBinding(),
      QuitOnErrorCallback(QuitLoopClosure(), "Page::SetSyncStateWatcher"));
}

void BacklogBenchmark::ConnectReader() {
  std::string reader_path = reader_tmp_dir_.path() + kUserDirectory;
  bool ret = files::CreateDirectory(reader_path);
  FXL_DCHECK(ret);

  cloud_provider::CloudProviderPtr cloud_provider_reader;
  cloud_provider_factory_.MakeCloudProviderWithGivenUserId(
      user_id_, cloud_provider_reader.NewRequest());
  GetLedger(startup_context_.get(), reader_controller_.NewRequest(),
            std::move(cloud_provider_reader), "backlog",
            DetachedPath(std::move(reader_path)), QuitLoopClosure(),
            [this](Status status, LedgerPtr reader) {
              if (QuitOnError(QuitLoopClosure(), status, "ConnectReader")) {
                return;
              }
              reader_ = std::move(reader);

              TRACE_ASYNC_BEGIN("benchmark", "download", 0);
              TRACE_ASYNC_BEGIN("benchmark", "get_reader_page", 0);
              reader_->GetPage(
                  fidl::MakeOptional(page_id_), reader_page_.NewRequest(),
                  [this](Status status) {
                    if (QuitOnError(QuitLoopClosure(), status, "GetPage")) {
                      return;
                    }
                    TRACE_ASYNC_END("benchmark", "get_reader_page", 0);
                    WaitForReaderDownload();
                  });
            });
}

void BacklogBenchmark::WaitForReaderDownload() {
  on_sync_state_changed_ = [this](SyncState download, SyncState upload) {
    if (download == SyncState::IDLE) {
      on_sync_state_changed_ = nullptr;
      TRACE_ASYNC_END("benchmark", "download", 0);
      GetReaderSnapshot();
      return;
    }
  };
  reader_page_->SetSyncStateWatcher(
      sync_watcher_binding_.NewBinding(),
      QuitOnErrorCallback(QuitLoopClosure(), "Page::SetSyncStateWatcher"));
}

void BacklogBenchmark::GetReaderSnapshot() {
  reader_page_->GetSnapshot(
      reader_snapshot_.NewRequest(), fidl::VectorPtr<uint8_t>::New(0), nullptr,
      QuitOnErrorCallback(QuitLoopClosure(), "GetSnapshot"));
  TRACE_ASYNC_BEGIN("benchmark", "get_all_entries", 0);
  GetEntriesStep(nullptr, unique_key_count_);
}

void BacklogBenchmark::CheckStatusAndGetMore(
    Status status, size_t entries_left, std::unique_ptr<Token> next_token) {
  if ((status != Status::OK) && (status != Status::PARTIAL_RESULT)) {
    if (QuitOnError(QuitLoopClosure(), status, "PageSnapshot::GetEntries")) {
      return;
    }
  }

  if (status == Status::OK) {
    TRACE_ASYNC_END("benchmark", "get_all_entries", 0);
    FXL_DCHECK(entries_left == 0);
    FXL_DCHECK(!next_token);
    ShutDown();
    RecordDirectorySize("uploader_directory_size", writer_tmp_dir_.path());
    RecordDirectorySize("reader_directory_size", reader_tmp_dir_.path());
    return;
  }
  FXL_DCHECK(next_token);
  GetEntriesStep(std::move(next_token), entries_left);
}

void BacklogBenchmark::GetEntriesStep(std::unique_ptr<Token> token,
                                      size_t entries_left) {
  FXL_DCHECK(entries_left > 0);
  TRACE_ASYNC_BEGIN("benchmark", "get_entries_partial", entries_left);
  if (reference_strategy_ == PageDataGenerator::ReferenceStrategy::INLINE) {
    reader_snapshot_->GetEntriesInline(
        fidl::VectorPtr<uint8_t>::New(0), std::move(token),
        [this, entries_left](Status status, auto entries,
                             auto next_token) mutable {
          TRACE_ASYNC_END("benchmark", "get_entries_partial", entries_left);
          CheckStatusAndGetMore(status, entries_left - entries->size(),
                                std::move(next_token));
        });
  } else {
    reader_snapshot_->GetEntries(
        fidl::VectorPtr<uint8_t>::New(0), std::move(token),
        [this, entries_left](Status status, auto entries,
                             auto next_token) mutable {
          TRACE_ASYNC_END("benchmark", "get_entries_partial", entries_left);
          CheckStatusAndGetMore(status, entries_left - entries->size(),
                                std::move(next_token));
        });
  }
}

void BacklogBenchmark::RecordDirectorySize(const std::string& event_name,
                                           const std::string& path) {
  uint64_t tmp_dir_size = 0;
  FXL_CHECK(GetDirectoryContentSize(DetachedPath(path), &tmp_dir_size));
  TRACE_COUNTER("benchmark", event_name.c_str(), 0, "directory_size",
                TA_UINT64(tmp_dir_size));
}

void BacklogBenchmark::ShutDown() {
  KillLedgerProcess(&uploader_controller_);
  KillLedgerProcess(&reader_controller_);
  loop_->Quit();
}

fit::closure BacklogBenchmark::QuitLoopClosure() {
  return [this] { loop_->Quit(); };
}

}  // namespace ledger

int main(int argc, const char** argv) {
  fxl::CommandLine command_line = fxl::CommandLineFromArgcArgv(argc, argv);

  std::string unique_key_count_str;
  size_t unique_key_count;
  std::string key_size_str;
  size_t key_size;
  std::string value_size_str;
  size_t value_size;
  std::string commit_count_str;
  size_t commit_count;
  std::string reference_strategy_str;
  ledger::SyncParams sync_params;
  if (!command_line.GetOptionValue(kUniqueKeyCountFlag.ToString(),
                                   &unique_key_count_str) ||
      !fxl::StringToNumberWithError(unique_key_count_str, &unique_key_count) ||
      unique_key_count <= 0 ||
      !command_line.GetOptionValue(kKeySizeFlag.ToString(), &key_size_str) ||
      !fxl::StringToNumberWithError(key_size_str, &key_size) || key_size == 0 ||
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
      !ledger::ParseSyncParamsFromCommandLine(command_line, &sync_params)) {
    PrintUsage();
    return -1;
  }

  ledger::PageDataGenerator::ReferenceStrategy reference_strategy;
  if (reference_strategy_str == kRefsOnFlag) {
    reference_strategy =
        ledger::PageDataGenerator::ReferenceStrategy::REFERENCE;
  } else if (reference_strategy_str == kRefsOffFlag) {
    reference_strategy = ledger::PageDataGenerator::ReferenceStrategy::INLINE;
  } else {
    std::cerr << "Unknown option " << reference_strategy_str << " for "
              << kRefsFlag.ToString() << std::endl;
    PrintUsage();
    return -1;
  }

  async::Loop loop(&kAsyncLoopConfigAttachToThread);
  ledger::BacklogBenchmark app(&loop, unique_key_count, key_size, value_size,
                               commit_count, reference_strategy,
                               std::move(sync_params));
  return ledger::RunWithTracing(&loop, [&app] { app.Run(); });
}
