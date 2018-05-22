// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/ledger/tests/benchmark/delete_entry/delete_entry.h"

#include <iostream>

#include <lib/async/cpp/task.h>
#include <lib/zx/time.h>
#include <trace/event.h>

#include "lib/callback/waiter.h"
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
constexpr fxl::StringView kStoragePath = "/data/benchmark/ledger/delete_entry";
constexpr fxl::StringView kEntryCountFlag = "entry-count";
constexpr fxl::StringView kTransactionSizeFlag = "transaction-size";
constexpr fxl::StringView kKeySizeFlag = "key-size";
constexpr fxl::StringView kValueSizeFlag = "value-size";

void PrintUsage(const char* executable_name) {
  std::cout << "Usage: " << executable_name << " --" << kEntryCountFlag
            << "=<int> --" << kTransactionSizeFlag << "=<int> --"
            << kKeySizeFlag << "=<int> --" << kValueSizeFlag << "=<int>"
            << std::endl;
}

}  // namespace

namespace test {
namespace benchmark {

DeleteEntryBenchmark::DeleteEntryBenchmark(async::Loop* loop,
                                           size_t entry_count,
                                           size_t transaction_size,
                                           size_t key_size, size_t value_size)
    : loop_(loop),
      tmp_dir_(kStoragePath),
      application_context_(
          component::ApplicationContext::CreateFromStartupInfo()),
      entry_count_(entry_count),
      transaction_size_(transaction_size),
      key_size_(key_size),
      value_size_(value_size) {
  FXL_DCHECK(loop_);
  FXL_DCHECK(entry_count_ > 0);
  FXL_DCHECK(transaction_size_ >= 0);
  FXL_DCHECK(key_size_ > 0);
  FXL_DCHECK(value_size_ > 0);
}

void DeleteEntryBenchmark::Run() {
  ledger::LedgerPtr ledger;
  ledger::Status status =
      test::GetLedger([this] { loop_->Quit(); }, application_context_.get(),
                      &application_controller_, nullptr, "delete_entry",
                      tmp_dir_.path(), &ledger);
  QuitOnError([this] { loop_->Quit(); }, status, "GetLedger");

  ledger::PageId id;
  status = test::GetPageEnsureInitialized([this] { loop_->Quit(); }, &ledger,
                                          nullptr, &page_, &id);
  QuitOnError([this] { loop_->Quit(); }, status, "Page initialization");

  Populate();
}

void DeleteEntryBenchmark::Populate() {
  auto keys = generator_.MakeKeys(entry_count_, key_size_, entry_count_);
  for (size_t i = 0; i < entry_count_; i++) {
    keys_.push_back(keys[i].Clone());
  }

  page_data_generator_.Populate(
      &page_, std::move(keys), value_size_, entry_count_,
      test::benchmark::PageDataGenerator::ReferenceStrategy::REFERENCE,
      ledger::Priority::EAGER, [this](ledger::Status status) {
        if (status != ledger::Status::OK) {
          benchmark::QuitOnError([this] { loop_->Quit(); }, status,
                                 "PageGenerator::Populate");
          return;
        }
        if (transaction_size_ > 0) {
          page_->StartTransaction([this](ledger::Status status) {
            if (benchmark::QuitOnError([this] { loop_->Quit(); }, status,
                                       "Page::StartTransaction")) {
              return;
            }
            TRACE_ASYNC_BEGIN("benchmark", "transaction", 0);
            RunSingle(0);
          });
        } else {
          RunSingle(0);
        }
      });
}

void DeleteEntryBenchmark::RunSingle(size_t i) {
  if (i == entry_count_) {
    ShutDown();
    return;
  }

  TRACE_ASYNC_BEGIN("benchmark", "delete_entry", i);
  page_->Delete(std::move(keys_[i]), [this, i](ledger::Status status) {
    if (benchmark::QuitOnError([this] { loop_->Quit(); }, status,
                               "Page::Delete")) {
      return;
    }
    TRACE_ASYNC_END("benchmark", "delete_entry", i);
    if (transaction_size_ > 0 &&
        (i % transaction_size_ == transaction_size_ - 1 ||
         i + 1 == entry_count_)) {
      CommitAndRunNext(i);
    } else {
      RunSingle(i + 1);
    }
  });
}

void DeleteEntryBenchmark::CommitAndRunNext(size_t i) {
  TRACE_ASYNC_BEGIN("benchmark", "commit", i / transaction_size_);
  page_->Commit([this, i](ledger::Status status) {
    if (benchmark::QuitOnError([this] { loop_->Quit(); }, status,
                               "Page::Commit")) {
      return;
    }
    TRACE_ASYNC_END("benchmark", "commit", i / transaction_size_);
    TRACE_ASYNC_END("benchmark", "transaction", i / transaction_size_);

    if (i == entry_count_ - 1) {
      RunSingle(i + 1);
      return;
    }
    page_->StartTransaction([this, i = i + 1](ledger::Status status) {
      if (benchmark::QuitOnError([this] { loop_->Quit(); }, status,
                                 "Page::StartTransaction")) {
        return;
      }
      TRACE_ASYNC_BEGIN("benchmark", "transaction", i / transaction_size_);
      RunSingle(i);
    });
  });
}

void DeleteEntryBenchmark::ShutDown() {
  application_controller_->Kill();
  application_controller_.WaitForResponseUntil(zx::deadline_after(zx::sec(5)));
  loop_->Quit();
}

}  // namespace benchmark
}  // namespace test

int main(int argc, const char** argv) {
  fxl::CommandLine command_line = fxl::CommandLineFromArgcArgv(argc, argv);

  std::string entry_count_str;
  size_t entry_count;
  std::string transaction_size_str;
  size_t transaction_size;
  std::string key_size_str;
  size_t key_size;
  std::string value_size_str;
  size_t value_size;
  if (!command_line.GetOptionValue(kEntryCountFlag.ToString(),
                                   &entry_count_str) ||
      !fxl::StringToNumberWithError(entry_count_str, &entry_count) ||
      entry_count == 0 ||
      !command_line.GetOptionValue(kTransactionSizeFlag.ToString(),
                                   &transaction_size_str) ||
      !fxl::StringToNumberWithError(transaction_size_str, &transaction_size) ||
      !command_line.GetOptionValue(kKeySizeFlag.ToString(), &key_size_str) ||
      !fxl::StringToNumberWithError(key_size_str, &key_size) || key_size == 0 ||
      !command_line.GetOptionValue(kValueSizeFlag.ToString(),
                                   &value_size_str) ||
      !fxl::StringToNumberWithError(value_size_str, &value_size) ||
      value_size == 0) {
    PrintUsage(argv[0]);
    return -1;
  }

  async::Loop loop(&kAsyncLoopConfigMakeDefault);
  test::benchmark::DeleteEntryBenchmark app(
      &loop, entry_count, transaction_size, key_size, value_size);

  return test::benchmark::RunWithTracing(&loop, [&app] { app.Run(); });
}
