// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/ledger/tests/integration/integration_test.h"

#include <trace/event.h>
#include <zx/time.h>

#include "garnet/lib/callback/auto_cleanable.h"
#include "garnet/lib/callback/capture.h"
#include "garnet/lib/callback/waiter.h"
#include "lib/fsl/vmo/vector.h"
#include "lib/fxl/functional/make_copyable.h"
#include "lib/fxl/memory/ref_ptr.h"
#include "lib/ledger/fidl/ledger.fidl.h"
#include "peridot/bin/ledger/storage/public/types.h"
#include "peridot/bin/ledger/testing/data_generator.h"
#include "peridot/bin/ledger/testing/get_ledger.h"
#include "peridot/lib/convert/convert.h"

namespace test {
namespace integration {
namespace sync {
namespace {

f1dl::VectorPtr<uint8_t> DoubleToArray(double dbl) {
  f1dl::VectorPtr<uint8_t> array = f1dl::VectorPtr<uint8_t>::New(sizeof(double));
  std::memcpy(array->data(), &dbl, sizeof(double));
  return array;
}

::testing::AssertionResult VmoToDouble(const fsl::SizedVmoTransportPtr& vmo,
                                       double* dbl) {
  if (vmo->size != sizeof(double)) {
    return ::testing::AssertionFailure()
           << "VMO has the wrong size: " << vmo->size << " instead of "
           << sizeof(double) << ".";
  }
  size_t num_read;
  zx_status_t status = vmo->vmo.read(dbl, 0, sizeof(double), &num_read);
  if (status < 0) {
    return ::testing::AssertionFailure() << "Unable to read the VMO.";
  }
  if (num_read != sizeof(double)) {
    return ::testing::AssertionFailure()
           << "VMO read of the wrong size: " << num_read << " instead of "
           << sizeof(double) << ".";
  }

  return ::testing::AssertionSuccess();
}

class RefCountedPageSnapshot
    : public fxl::RefCountedThreadSafe<RefCountedPageSnapshot> {
 public:
  RefCountedPageSnapshot() {}

  ledger::PageSnapshotPtr& operator->() { return snapshot_; }
  ledger::PageSnapshotPtr& operator*() { return snapshot_; }

 private:
  ledger::PageSnapshotPtr snapshot_;
};

class PageWatcherImpl : public ledger::PageWatcher {
 public:
  PageWatcherImpl(f1dl::InterfaceRequest<ledger::PageWatcher> request,
                  fxl::RefPtr<RefCountedPageSnapshot> base_snapshot)
      : binding_(this, std::move(request)),
        current_snapshot_(std::move(base_snapshot)) {}

  int changes = 0;

  void GetInlineOnLatestSnapshot(
      f1dl::VectorPtr<uint8_t> key,
      ledger::PageSnapshot::GetInlineCallback callback) {
    // We need to make sure the PageSnapshotPtr used to make the |GetInline|
    // call survives as long as the call is active, even if a new snapshot
    // arrives in between.
    (*current_snapshot_)
        ->GetInline(std::move(key), [snapshot = current_snapshot_.Clone(),
                                     callback = std::move(callback)](
                                        ledger::Status status,
                                        f1dl::VectorPtr<uint8_t> value) mutable {
          callback(status, std::move(value));
        });
  }

 private:
  // PageWatcher:
  void OnChange(ledger::PageChangePtr /*page_change*/,
                ledger::ResultState /*result_state*/,
                const OnChangeCallback& callback) override {
    changes++;
    current_snapshot_ = fxl::AdoptRef(new RefCountedPageSnapshot());
    callback((**current_snapshot_).NewRequest());
  }

  f1dl::Binding<ledger::PageWatcher> binding_;
  fxl::RefPtr<RefCountedPageSnapshot> current_snapshot_;

  FXL_DISALLOW_COPY_AND_ASSIGN(PageWatcherImpl);
};

class SyncWatcherImpl : public ledger::SyncWatcher {
 public:
  SyncWatcherImpl() : binding_(this) {}

  auto NewBinding() { return binding_.NewBinding(); }

  bool new_state = false;
  ledger::SyncState download;
  ledger::SyncState upload;

 private:
  // SyncWatcher
  void SyncStateChanged(ledger::SyncState download,
                        ledger::SyncState upload,
                        const SyncStateChangedCallback& callback) override {
    this->download = download;
    this->upload = upload;
    new_state = true;
    callback();
  }

  f1dl::Binding<ledger::SyncWatcher> binding_;

  FXL_DISALLOW_COPY_AND_ASSIGN(SyncWatcherImpl);
};

// NonAssociativeConflictResolverImpl uses a merge function which is neither
// associative nor commutative. This means that merging ((1, 2), 3) results in
// a different value than merging ((2, 3), 1), or ((2, 1), 3).
// This conflict resolver only works on numeric data. For values A and B, it
// produces the merged value (4*A+B)/3.
class NonAssociativeConflictResolverImpl : public ledger::ConflictResolver {
 public:
  explicit NonAssociativeConflictResolverImpl(
      f1dl::InterfaceRequest<ledger::ConflictResolver> request)
      : binding_(this, std::move(request)) {}
  ~NonAssociativeConflictResolverImpl() override {}

 private:
  // ConflictResolver:
  void Resolve(f1dl::InterfaceHandle<ledger::PageSnapshot> /*left_version*/,
               f1dl::InterfaceHandle<ledger::PageSnapshot> /*right_version*/,
               f1dl::InterfaceHandle<ledger::PageSnapshot> /*common_version*/,
               f1dl::InterfaceHandle<ledger::MergeResultProvider>
                   result_provider) override {
    ledger::MergeResultProviderPtr merge_result_provider =
        result_provider.Bind();
    ledger::MergeResultProvider* merge_result_provider_ptr =
        merge_result_provider.get();
    merge_result_provider_ptr->GetFullDiff(
        nullptr,
        fxl::MakeCopyable([merge_result_provider =
                               std::move(merge_result_provider)](
                              ledger::Status status,
                              f1dl::VectorPtr<ledger::DiffEntryPtr> changes,
                              f1dl::VectorPtr<uint8_t> next_token) mutable {
          ASSERT_EQ(ledger::Status::OK, status);
          ASSERT_EQ(1u, changes->size());

          double d1, d2;
          EXPECT_TRUE(VmoToDouble(changes->at(0)->left->value, &d1));
          EXPECT_TRUE(VmoToDouble(changes->at(0)->right->value, &d2));
          double new_value = (4 * d1 + d2) / 3;
          ledger::MergedValuePtr merged_value = ledger::MergedValue::New();
          merged_value->key = std::move(changes->at(0)->key);
          merged_value->source = ledger::ValueSource::NEW;
          merged_value->new_value = ledger::BytesOrReference::New();
          merged_value->new_value->set_bytes(DoubleToArray(new_value));
          f1dl::VectorPtr<ledger::MergedValuePtr> merged_values;
          merged_values.push_back(std::move(merged_value));
          ledger::Status merge_status;
          merge_result_provider->Merge(std::move(merged_values),
                                       callback::Capture([] {}, &merge_status));
          ASSERT_TRUE(merge_result_provider.WaitForResponse());
          ASSERT_EQ(ledger::Status::OK, merge_status);
          merge_result_provider->Done(callback::Capture([] {}, &merge_status));
          ASSERT_TRUE(merge_result_provider.WaitForResponse());
          ASSERT_EQ(ledger::Status::OK, merge_status);
        }));
  }

  f1dl::Binding<ledger::ConflictResolver> binding_;
};

class TestConflictResolverFactory : public ledger::ConflictResolverFactory {
 public:
  explicit TestConflictResolverFactory(
      f1dl::InterfaceRequest<ledger::ConflictResolverFactory> request)
      : binding_(this, std::move(request)) {}

 private:
  // ConflictResolverFactory:
  void GetPolicy(f1dl::VectorPtr<uint8_t> /*page_id*/,
                 const GetPolicyCallback& callback) override {
    callback(ledger::MergePolicy::CUSTOM);
  }

  void NewConflictResolver(
      f1dl::VectorPtr<uint8_t> page_id,
      f1dl::InterfaceRequest<ledger::ConflictResolver> resolver) override {
    resolvers.emplace(std::piecewise_construct,
                      std::forward_as_tuple(convert::ToString(page_id)),
                      std::forward_as_tuple(std::move(resolver)));
  }

  std::map<storage::PageId, NonAssociativeConflictResolverImpl> resolvers;

  f1dl::Binding<ledger::ConflictResolverFactory> binding_;
};

enum class MergeType {
  LAST_ONE_WINS,
  NON_ASSOCIATIVE_CUSTOM,
};

class ConvergenceTest
    : public BaseIntegrationTest,
      public ::testing::WithParamInterface<
          std::tuple<MergeType, int, LedgerAppInstanceFactory*>> {
 public:
  ConvergenceTest(){};
  ~ConvergenceTest() override{};

  void SetUp() override {
    BaseIntegrationTest::SetUp();
    LedgerAppInstanceFactory* not_used;
    std::tie(merge_function_type_, num_ledgers_, not_used) = GetParam();

    ASSERT_GT(num_ledgers_, 1);

    f1dl::VectorPtr<uint8_t> page_id;
    for (int i = 0; i < num_ledgers_; i++) {
      auto ledger_instance = NewLedgerAppInstance();
      ASSERT_TRUE(ledger_instance);
      ledger_instances_.push_back(std::move(ledger_instance));
      pages_.emplace_back();
      ledger::LedgerPtr ledger_ptr = ledger_instances_[i]->GetTestLedger();
      ledger::Status status = test::GetPageEnsureInitialized(
          &message_loop_, &ledger_ptr,
          // The first ledger gets a random page id, the others use the
          // same id for their pages.
          i == 0 ? nullptr : page_id.Clone(), &pages_[i], &page_id);
      ASSERT_EQ(ledger::Status::OK, status);
    }
  }

 protected:
  LedgerAppInstanceFactory* GetAppFactory() override {
    return std::get<2>(GetParam());
  }

  std::unique_ptr<PageWatcherImpl> WatchPageContents(ledger::PagePtr* page) {
    ledger::PageWatcherPtr page_watcher;
    fxl::RefPtr<RefCountedPageSnapshot> page_snapshot =
        fxl::AdoptRef(new RefCountedPageSnapshot());
    f1dl::InterfaceRequest<ledger::PageSnapshot> page_snapshot_request =
        (**page_snapshot).NewRequest();
    std::unique_ptr<PageWatcherImpl> watcher =
        std::make_unique<PageWatcherImpl>(page_watcher.NewRequest(),
                                          std::move(page_snapshot));
    ledger::Status status = ledger::Status::UNKNOWN_ERROR;
    (*page)->GetSnapshot(std::move(page_snapshot_request), nullptr,
                         std::move(page_watcher),
                         callback::Capture(MakeQuitTask(), &status));
    RunLoop();
    EXPECT_EQ(ledger::Status::OK, status);
    return watcher;
  }

  std::unique_ptr<SyncWatcherImpl> WatchPageSyncState(ledger::PagePtr* page) {
    std::unique_ptr<SyncWatcherImpl> watcher =
        std::make_unique<SyncWatcherImpl>();
    ledger::Status status = ledger::Status::UNKNOWN_ERROR;
    (*page)->SetSyncStateWatcher(watcher->NewBinding(),
                                 callback::Capture(MakeQuitTask(), &status));
    RunLoop();
    EXPECT_EQ(ledger::Status::OK, status);
    return watcher;
  }

  // Returns true if the values for |key| on all the watchers are identical.
  bool AreValuesIdentical(
      const std::vector<std::unique_ptr<PageWatcherImpl>>& watchers,
      std::string key) {
    std::vector<f1dl::VectorPtr<uint8_t>> values;
    for (int i = 0; i < num_ledgers_; i++) {
      values.emplace_back();
      ledger::Status status = ledger::Status::UNKNOWN_ERROR;
      watchers[i]->GetInlineOnLatestSnapshot(
          convert::ToArray(key),
          callback::Capture(MakeQuitTask(), &status, &values[i]));
      RunLoop();
      EXPECT_EQ(ledger::Status::OK, status);
    }

    bool values_are_identical = true;
    for (int i = 1; i < num_ledgers_; i++) {
      values_are_identical &= convert::ExtendedStringView(values[0]) ==
                              convert::ExtendedStringView(values[i]);
    }
    return values_are_identical;
  }

  int num_ledgers_;
  MergeType merge_function_type_;

  std::vector<std::unique_ptr<LedgerAppInstanceFactory::LedgerAppInstance>>
      ledger_instances_;
  std::vector<ledger::PagePtr> pages_;
  test::DataGenerator data_generator_;
};

// Verify that the Ledger converges over different settings of merging functions
// and number of ledger instances.
TEST_P(ConvergenceTest, NLedgersConverge) {
  std::vector<std::unique_ptr<PageWatcherImpl>> watchers;
  std::vector<std::unique_ptr<SyncWatcherImpl>> sync_watchers;

  std::vector<std::unique_ptr<TestConflictResolverFactory>> resolver_factories;
  std::independent_bits_engine<std::default_random_engine, CHAR_BIT, uint8_t>
      generator;
  std::uniform_real_distribution<> distribution(1, 100);

  for (int i = 0; i < num_ledgers_; i++) {
    ledger::Status status = ledger::Status::UNKNOWN_ERROR;
    if (merge_function_type_ == MergeType::NON_ASSOCIATIVE_CUSTOM) {
      ledger::ConflictResolverFactoryPtr resolver_factory_ptr;
      resolver_factories.push_back(
          std::make_unique<TestConflictResolverFactory>(
              resolver_factory_ptr.NewRequest()));
      ledger::LedgerPtr ledger = ledger_instances_[i]->GetTestLedger();
      ledger->SetConflictResolverFactory(
          std::move(resolver_factory_ptr),
          callback::Capture(MakeQuitTask(), &status));
      RunLoop();
      EXPECT_EQ(ledger::Status::OK, status);
    }

    watchers.push_back(WatchPageContents(&pages_[i]));
    sync_watchers.push_back(WatchPageSyncState(&pages_[i]));

    pages_[i]->StartTransaction(callback::Capture(MakeQuitTask(), &status));
    RunLoop();
    EXPECT_EQ(ledger::Status::OK, status);

    if (merge_function_type_ == MergeType::NON_ASSOCIATIVE_CUSTOM) {
      pages_[i]->Put(convert::ToArray("value"),
                     DoubleToArray(distribution(generator)),
                     callback::Capture(MakeQuitTask(), &status));
    } else {
      pages_[i]->Put(convert::ToArray("value"), data_generator_.MakeValue(50),
                     callback::Capture(MakeQuitTask(), &status));
    }
    RunLoop();
    EXPECT_EQ(ledger::Status::OK, status);
  }

  auto commit_waiter =
      callback::StatusWaiter<ledger::Status>::Create(ledger::Status::OK);
  ledger::Status status = ledger::Status::UNKNOWN_ERROR;
  for (int i = 0; i < num_ledgers_; i++) {
    pages_[i]->Commit(commit_waiter->NewCallback());
  }
  commit_waiter->Finalize(callback::Capture(MakeQuitTask(), &status));
  RunLoop();

  // Function to verify if the visible Ledger state has not changed since last
  // call and all values are identical.
  std::function<bool()> has_state_converged = [this, &watchers,
                                               &sync_watchers]() {
    // Counts the number of visible changes. Used to verify that the minimal
    // number of changes for all Ledgers to have communicated is accounted for
    // (see also below).
    int num_changes = 0;
    for (int i = 0; i < num_ledgers_; i++) {
      num_changes += watchers[i]->changes;
    }
    // All ledgers should see their own change (num_ledgers_). Then, at least
    // all but one should receive a change with the "final" value. There might
    // be more changes seen, though.
    if (num_changes < 2 * num_ledgers_ - 1) {
      return false;
    }

    // All synchronization must be idle.
    bool idle = true;
    for (int i = 0; i < num_ledgers_; i++) {
      if (sync_watchers[i]->download != ledger::SyncState::IDLE ||
          sync_watchers[i]->upload != ledger::SyncState::IDLE ||
          sync_watchers[i]->new_state) {
        idle = false;
      }
      // With this, we make sure we can verify if the state changes during the
      // next cycle. If it has changed, we are sure the convergence has not
      // happened yet.
      sync_watchers[i]->new_state = false;
    }

    return idle && AreValuesIdentical(watchers, "value");
  };

  bool merge_done = false;
  ledger::ConflictResolutionWaitStatus wait_status =
      ledger::ConflictResolutionWaitStatus::NO_CONFLICTS;
  fxl::RefPtr<callback::StatusWaiter<ledger::ConflictResolutionWaitStatus>>
      waiter;

  // In addition of verifying that the external states of the ledgers have
  // converged, we also verify we are not currently performing a merge in the
  // background, indicating that the convergence did not finish.
  std::function<bool()> is_sync_and_merge_complete = [this,
                                                      &has_state_converged,
                                                      &merge_done, &wait_status,
                                                      &waiter] {
    TRACE_DURATION("ledger", "ledger_test_is_sync_and_merge_complete");

    if (has_state_converged()) {
      if (merge_done &&
          wait_status == ledger::ConflictResolutionWaitStatus::NO_CONFLICTS) {
        return true;
      }
      if (!waiter) {
        waiter = callback::StatusWaiter<ledger::ConflictResolutionWaitStatus>::
            Create(ledger::ConflictResolutionWaitStatus::NO_CONFLICTS);
        for (int i = 0; i < num_ledgers_; i++) {
          pages_[i]->WaitForConflictResolution(waiter->NewCallback());
        }
        waiter->Finalize([&merge_done, &wait_status, &waiter](
                             ledger::ConflictResolutionWaitStatus status) {
          merge_done = true;
          wait_status = status;
          waiter = nullptr;
        });
      }
      return false;
    } else {
      merge_done = false;
      if (waiter) {
        waiter->Cancel();
        waiter = nullptr;
      }
      return false;
    }
  };

  // If |RunLoopUntil| returns, the condition is met, thus the ledgers have
  // converged.
  RunLoopUntil(is_sync_and_merge_complete);
  int num_changes = 0;
  for (int i = 0; i < num_ledgers_; i++) {
    num_changes += watchers[i]->changes;
  }
  EXPECT_GE(num_changes, 2 * num_ledgers_ - 1);

  // All synchronization must still be idle.
  for (int i = 0; i < num_ledgers_; i++) {
    EXPECT_FALSE(sync_watchers[i]->new_state);
    EXPECT_EQ(ledger::SyncState::IDLE, sync_watchers[i]->download);
    EXPECT_EQ(ledger::SyncState::IDLE, sync_watchers[i]->upload);
  }

  EXPECT_TRUE(AreValuesIdentical(watchers, "value"));
}

INSTANTIATE_TEST_CASE_P(
    ManyLedgersConvergenceTest,
    ConvergenceTest,
    ::testing::Combine(::testing::Values(MergeType::LAST_ONE_WINS,
                                         MergeType::NON_ASSOCIATIVE_CUSTOM),
                       ::testing::Range(2, 6),
                       ::testing::ValuesIn(GetLedgerAppInstanceFactories())));

}  // namespace
}  // namespace sync
}  // namespace integration
}  // namespace test
