// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/ledger/tests/benchmark/convergence/convergence.h"

#include <trace/event.h>
#include <zx/time.h>

#include <iostream>

#include "garnet/lib/callback/waiter.h"
#include "lib/fsl/tasks/message_loop.h"
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
constexpr fxl::StringView kEntryCountFlag = "entry-count";
constexpr fxl::StringView kValueSizeFlag = "value-size";
constexpr fxl::StringView kServerIdFlag = "server-id";

void PrintUsage(const char* executable_name) {
  std::cout << "Usage: " << executable_name << " --" << kEntryCountFlag
            << "=<int> --" << kValueSizeFlag << "=<int> --" << kServerIdFlag
            << "=<string>" << std::endl;
}

constexpr size_t kKeySize = 100;

}  // namespace

namespace test {
namespace benchmark {

ConvergenceBenchmark::ConvergenceBenchmark(int entry_count, int value_size,
                                           std::string server_id)
    : application_context_(
          component::ApplicationContext::CreateFromStartupInfo()),
      cloud_provider_firebase_factory_(application_context_.get()),
      entry_count_(entry_count),
      value_size_(value_size),
      server_id_(std::move(server_id)),
      alpha_watcher_binding_(this),
      beta_watcher_binding_(this),
      alpha_tmp_dir_(kStoragePath),
      beta_tmp_dir_(kStoragePath) {
  FXL_DCHECK(entry_count > 0);
  FXL_DCHECK(value_size > 0);
  cloud_provider_firebase_factory_.Init();
}

void ConvergenceBenchmark::Run() {
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
  ledger::Status status = test::GetLedger(
      fsl::MessageLoop::GetCurrent(), application_context_.get(),
      &alpha_controller_, std::move(cloud_provider_alpha), "sync", alpha_path,
      &alpha_ledger_);
  QuitOnError(status, "alpha ledger");

  cloud_provider::CloudProviderPtr cloud_provider_beta;
  cloud_provider_firebase_factory_.MakeCloudProvider(
      server_id_, "", cloud_provider_beta.NewRequest());
  status = test::GetLedger(fsl::MessageLoop::GetCurrent(),
                           application_context_.get(), &beta_controller_,
                           std::move(cloud_provider_beta), "sync", beta_path,
                           &beta_ledger_);
  QuitOnError(status, "beta ledger");

  ledger::PagePtr page;
  f1dl::Array<uint8_t> id;
  status = test::GetPageEnsureInitialized(fsl::MessageLoop::GetCurrent(),
                                          &alpha_ledger_, nullptr, &page, &id);
  QuitOnError(status, "alpha page initialization");
  page_id_ = id.Clone();
  alpha_page_ = std::move(page);
  beta_ledger_->GetPage(std::move(id), beta_page_.NewRequest(),
                        benchmark::QuitOnErrorCallback("GetPage"));

  // Register both watchers. We don't actually need the snapshots.
  auto waiter =
      callback::StatusWaiter<ledger::Status>::Create(ledger::Status::OK);
  ledger::PageSnapshotPtr alpha_snapshot;
  alpha_page_->GetSnapshot(alpha_snapshot.NewRequest(), nullptr,
                           alpha_watcher_binding_.NewBinding(),
                           waiter->NewCallback());
  ledger::PageSnapshotPtr beta_snapshot;
  beta_page_->GetSnapshot(beta_snapshot.NewRequest(), nullptr,
                          beta_watcher_binding_.NewBinding(),
                          waiter->NewCallback());
  waiter->Finalize([this](ledger::Status status) {
    if (benchmark::QuitOnError(status, "GetSnapshot")) {
      return;
    }
    Start(0);
  });
}

void ConvergenceBenchmark::Start(int step) {
  if (step == entry_count_) {
    ShutDown();
    return;
  }

  {
    f1dl::Array<uint8_t> key = generator_.MakeKey(2 * step, kKeySize);
    // Insert each key twice, as we will receive two notifications - one on the
    // sender side (each page client sees their own changes), and one on the
    // receiving side.
    remaining_keys_.insert(convert::ToString(key));
    remaining_keys_.insert(convert::ToString(key));
    f1dl::Array<uint8_t> value = generator_.MakeValue(value_size_);
    alpha_page_->Put(std::move(key), std::move(value),
                     benchmark::QuitOnErrorCallback("Put"));
  }

  {
    f1dl::Array<uint8_t> key = generator_.MakeKey(2 * step + 1, kKeySize);
    // Insert each key twice, as we will receive two notifications - one on the
    // sender side (each page client sees their own changes), and one on the
    // receiving side.
    remaining_keys_.insert(convert::ToString(key));
    remaining_keys_.insert(convert::ToString(key));
    f1dl::Array<uint8_t> value = generator_.MakeValue(value_size_);
    beta_page_->Put(std::move(key), std::move(value),
                    benchmark::QuitOnErrorCallback("Put"));
  }

  TRACE_ASYNC_BEGIN("benchmark", "convergence", step);
  // Persist the current step, so that we know which async event to end in
  // OnChange().
  current_step_ = step;
}

void ConvergenceBenchmark::OnChange(ledger::PageChangePtr page_change,
                                    ledger::ResultState result_state,
                                    const OnChangeCallback& callback) {
  FXL_DCHECK(result_state == ledger::ResultState::COMPLETED);
  for (auto& change : *page_change->changed_entries) {
    auto find_one = remaining_keys_.find(convert::ToString(change->key));
    remaining_keys_.erase(find_one);
  }
  if (remaining_keys_.empty()) {
    TRACE_ASYNC_END("benchmark", "convergence", current_step_);
    Start(current_step_ + 1);
  }
  callback(nullptr);
}

void ConvergenceBenchmark::ShutDown() {
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

  std::string entry_count_str;
  int entry_count;
  std::string value_size_str;
  int value_size;
  std::string server_id;
  if (!command_line.GetOptionValue(kEntryCountFlag.ToString(),
                                   &entry_count_str) ||
      !fxl::StringToNumberWithError(entry_count_str, &entry_count) ||
      entry_count <= 0 ||
      !command_line.GetOptionValue(kValueSizeFlag.ToString(),
                                   &value_size_str) ||
      !fxl::StringToNumberWithError(value_size_str, &value_size) ||
      value_size <= 0 ||
      !command_line.GetOptionValue(kServerIdFlag.ToString(), &server_id)) {
    PrintUsage(argv[0]);
    return -1;
  }

  fsl::MessageLoop loop;
  test::benchmark::ConvergenceBenchmark app(entry_count, value_size, server_id);
  return test::benchmark::RunWithTracing(&loop, [&app] { app.Run(); });
}
