// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/ledger/tests/benchmark/sync/sync.h"

#include <trace/event.h>
#include <zx/time.h>

#include <iostream>

#include "lib/cloud_provider/fidl/cloud_provider.fidl.h"
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
constexpr fxl::StringView kStoragePath = "/data/benchmark/ledger/sync";
constexpr fxl::StringView kChangeCountFlag = "change-count";
constexpr fxl::StringView kValueSizeFlag = "value-size";
constexpr fxl::StringView kEntriesPerChangeFlag = "entries-per-change";
constexpr fxl::StringView kRefsFlag = "refs";
constexpr fxl::StringView kServerIdFlag = "server-id";

constexpr fxl::StringView kRefsOnFlag = "on";
constexpr fxl::StringView kRefsOffFlag = "off";

constexpr size_t kKeySize = 100;

void PrintUsage(const char* executable_name) {
  std::cout << "Usage: " << executable_name << " --" << kChangeCountFlag
            << "=<int> --" << kValueSizeFlag << "=<int> --"
            << kEntriesPerChangeFlag << "=<int> --" << kRefsFlag << "=("
            << kRefsOnFlag << "|" << kRefsOffFlag << ") --" << kServerIdFlag
            << "=<string>" << std::endl;
}

}  // namespace

namespace test {
namespace benchmark {

SyncBenchmark::SyncBenchmark(
    size_t change_count, size_t value_size, size_t entries_per_change,
    PageDataGenerator::ReferenceStrategy reference_strategy,
    std::string server_id)
    : application_context_(
          component::ApplicationContext::CreateFromStartupInfo()),
      cloud_provider_firebase_factory_(application_context_.get()),
      change_count_(change_count),
      value_size_(value_size),
      entries_per_change_(entries_per_change),
      reference_strategy_(reference_strategy),
      server_id_(std::move(server_id)),
      page_watcher_binding_(this),
      alpha_tmp_dir_(kStoragePath),
      beta_tmp_dir_(kStoragePath) {
  FXL_DCHECK(change_count_ > 0);
  FXL_DCHECK(value_size_ > 0);
  FXL_DCHECK(entries_per_change_ > 0);
  cloud_provider_firebase_factory_.Init();
}

void SyncBenchmark::Run() {
  // Name of the storage directory currently identifies the user. Ensure the
  // most nested directory has the same name to make the ledgers sync.
  std::string alpha_path = alpha_tmp_dir_.path() + "/sync_user";
  bool ret = files::CreateDirectory(alpha_path);
  FXL_DCHECK(ret);

  std::string beta_path = beta_tmp_dir_.path() + "/sync_user";
  ret = files::CreateDirectory(beta_path);
  FXL_DCHECK(ret);

  cloud_provider::CloudProviderPtr cloud_provider_alpha;
  cloud_provider_firebase_factory_.MakeCloudProvider(
      server_id_, "", cloud_provider_alpha.NewRequest());
  ledger::LedgerPtr alpha;
  ledger::Status status = test::GetLedger(
      fsl::MessageLoop::GetCurrent(), application_context_.get(),
      &alpha_controller_, std::move(cloud_provider_alpha), "sync", alpha_path,
      &alpha);
  QuitOnError(status, "alpha ledger");

  cloud_provider::CloudProviderPtr cloud_provider_beta;
  cloud_provider_firebase_factory_.MakeCloudProvider(
      server_id_, "", cloud_provider_beta.NewRequest());
  ledger::LedgerPtr beta;
  status =
      test::GetLedger(fsl::MessageLoop::GetCurrent(),
                      application_context_.get(), &beta_controller_,
                      std::move(cloud_provider_beta), "sync", beta_path, &beta);
  QuitOnError(status, "beta ledger");

  f1dl::Array<uint8_t> id;
  status = test::GetPageEnsureInitialized(fsl::MessageLoop::GetCurrent(),
                                          &alpha, nullptr, &alpha_page_, &id);
  QuitOnError(status, "alpha page initialization");
  page_id_ = id.Clone();
  beta->GetPage(std::move(id), beta_page_.NewRequest(),
                benchmark::QuitOnErrorCallback("GetPage"));

  ledger::PageSnapshotPtr snapshot;
  beta_page_->GetSnapshot(snapshot.NewRequest(), nullptr,
                          page_watcher_binding_.NewBinding(),
                          [this](ledger::Status status) {
                            if (benchmark::QuitOnError(status, "GetSnapshot")) {
                              return;
                            }
                            RunSingleChange(0);
                          });
}

void SyncBenchmark::OnChange(ledger::PageChangePtr page_change,
                             ledger::ResultState result_state,
                             const OnChangeCallback& callback) {
  FXL_DCHECK(page_change->changed_entries->size() > 0);
  size_t i =
      std::stoul(convert::ToString(page_change->changed_entries[0]->key));
  changed_entries_received_ += page_change->changed_entries.size();
  if (result_state == ledger::ResultState::COMPLETED ||
      result_state == ledger::ResultState::PARTIAL_STARTED) {
    TRACE_ASYNC_END("benchmark", "sync latency", i);
  }
  if (result_state == ledger::ResultState::COMPLETED ||
      result_state == ledger::ResultState::PARTIAL_COMPLETED) {
    FXL_DCHECK(changed_entries_received_ == entries_per_change_);
    RunSingleChange(i + 1);
  }
  callback(nullptr);
}

void SyncBenchmark::RunSingleChange(size_t change_number) {
  if (change_number == change_count_) {
    ShutDown();
    return;
  }

  std::vector<f1dl::Array<uint8_t>> keys(entries_per_change_);
  for (size_t i = 0; i < entries_per_change_; i++) {
    // Keys are distinct, but have the common prefix <i>.
    keys[i] = generator_.MakeKey(change_number, kKeySize);
  }

  changed_entries_received_ = 0;
  TRACE_ASYNC_BEGIN("benchmark", "sync latency", change_number);
  page_data_generator_.Populate(
      &alpha_page_, std::move(keys), value_size_, entries_per_change_,
      reference_strategy_, ledger::Priority::EAGER, [](ledger::Status status) {
        if (benchmark::QuitOnError(status, "PageDataGenerator::Populate")) {
          return;
        }
      });
}

void SyncBenchmark::ShutDown() {
  alpha_controller_->Kill();
  alpha_controller_.WaitForResponseUntil(zx::deadline_after(zx::sec(5)));
  beta_controller_->Kill();
  beta_controller_.WaitForResponseUntil(zx::deadline_after(zx::sec(5)));

  fsl::MessageLoop::GetCurrent()->PostQuitTask();
}
}  // namespace benchmark
}  // namespace test

int main(int argc, const char** argv) {
  fxl::CommandLine command_line = fxl::CommandLineFromArgcArgv(argc, argv);

  std::string change_count_str;
  size_t change_count;
  std::string value_size_str;
  size_t value_size;
  std::string entries_per_change_str;
  size_t entries_per_change;
  std::string reference_strategy_str;
  std::string server_id;
  if (!command_line.GetOptionValue(kChangeCountFlag.ToString(),
                                   &change_count_str) ||
      !fxl::StringToNumberWithError(change_count_str, &change_count) ||
      change_count <= 0 ||
      !command_line.GetOptionValue(kValueSizeFlag.ToString(),
                                   &value_size_str) ||
      !fxl::StringToNumberWithError(value_size_str, &value_size) ||
      value_size <= 0 ||
      !command_line.GetOptionValue(kEntriesPerChangeFlag.ToString(),
                                   &entries_per_change_str) ||
      !fxl::StringToNumberWithError(entries_per_change_str,
                                    &entries_per_change) ||
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
  test::benchmark::SyncBenchmark app(change_count, value_size,
                                     entries_per_change, reference_strategy,
                                     server_id);
  return test::benchmark::RunWithTracing(&loop, [&app] { app.Run(); });
}
