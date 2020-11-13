// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/forensics/crash_reports/queue.h"

#include <lib/syslog/cpp/macros.h>
#include <lib/zx/time.h>

#include <memory>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/developer/forensics/crash_reports/constants.h"
#include "src/developer/forensics/crash_reports/info/info_context.h"
#include "src/developer/forensics/crash_reports/network_watcher.h"
#include "src/developer/forensics/crash_reports/reporting_policy_watcher.h"
#include "src/developer/forensics/crash_reports/tests/stub_crash_server.h"
#include "src/developer/forensics/testing/stubs/cobalt_logger_factory.h"
#include "src/developer/forensics/testing/stubs/network_reachability_provider.h"
#include "src/developer/forensics/testing/unit_test_fixture.h"
#include "src/lib/files/directory.h"
#include "src/lib/files/file.h"
#include "src/lib/files/path.h"
#include "src/lib/files/scoped_temp_dir.h"
#include "src/lib/fsl/vmo/strings.h"
#include "src/lib/fxl/strings/string_printf.h"
#include "src/lib/timekeeper/test_clock.h"

namespace forensics {
namespace crash_reports {
namespace {

using inspect::testing::ChildrenMatch;
using inspect::testing::NameMatches;
using inspect::testing::NodeMatches;
using inspect::testing::PropertyList;
using inspect::testing::StringIs;
using inspect::testing::UintIs;
using testing::Contains;
using testing::ElementsAre;
using testing::IsEmpty;
using testing::IsSupersetOf;
using testing::Not;
using testing::Pair;
using testing::UnorderedElementsAre;
using testing::UnorderedElementsAreArray;

constexpr CrashServer::UploadStatus kUploadSuccessful = CrashServer::UploadStatus::kSuccess;
constexpr CrashServer::UploadStatus kUploadFailed = CrashServer::UploadStatus::kFailure;
constexpr CrashServer::UploadStatus kUploadThrottled = CrashServer::UploadStatus::kThrottled;

constexpr char kStorePath[] = "/tmp/reports";

constexpr char kAttachmentKey[] = "attachment.key";
constexpr char kAttachmentValue[] = "attachment.value";
constexpr char kAnnotationKey[] = "annotation.key";
constexpr char kAnnotationValue[] = "annotation.value";
constexpr char kSnapshotUuidValue[] = "snapshot_uuid";
constexpr char kMinidumpKey[] = "uploadFileMinidump";
constexpr char kMinidumpValue[] = "minidump";

constexpr zx::duration kPeriodicUploadDuration = zx::min(15);

fuchsia::mem::Buffer BuildAttachment(const std::string& value) {
  fuchsia::mem::Buffer attachment;
  FX_CHECK(fsl::VmoFromString(value, &attachment));
  return attachment;
}

std::map<std::string, fuchsia::mem::Buffer> MakeAttachments() {
  std::map<std::string, fuchsia::mem::Buffer> attachments;
  attachments[kAttachmentKey] = BuildAttachment(kAttachmentValue);
  return attachments;
}

std::optional<std::string> DeleteReportFromStore() {
  auto RemoveCurDir = [](std::vector<std::string>* contents) {
    contents->erase(std::remove(contents->begin(), contents->end(), "."), contents->end());
  };

  std::vector<std::string> program_shortnames;
  files::ReadDirContents(kStorePath, &program_shortnames);
  RemoveCurDir(&program_shortnames);
  for (const auto& program_shortname : program_shortnames) {
    const std::string path = files::JoinPath(kStorePath, program_shortname);

    std::vector<std::string> report_ids;
    files::ReadDirContents(path, &report_ids);
    RemoveCurDir(&report_ids);

    if (!report_ids.empty()) {
      files::DeletePath(files::JoinPath(path, report_ids.back()), /*recursive=*/true);
      return report_ids.back();
    }
  }
  return std::nullopt;
}

std::map<std::string, std::string> MakeAnnotations() {
  return {{kAnnotationKey, kAnnotationValue}};
}

Report MakeReport(const std::size_t report_id) {
  std::optional<Report> report =
      Report::MakeReport(report_id, fxl::StringPrintf("program_%ld", report_id), MakeAnnotations(),
                         MakeAttachments(), kSnapshotUuidValue, BuildAttachment(kMinidumpValue));
  FX_CHECK(report.has_value());
  return std::move(report.value());
}

class TestReportingPolicyWatcher : public ReportingPolicyWatcher {
 public:
  TestReportingPolicyWatcher() : ReportingPolicyWatcher(ReportingPolicy::kUndecided) {}

  void Set(const ReportingPolicy policy) { SetPolicy(policy); }
};

class QueueTest : public UnitTestFixture {
 public:
  QueueTest() : network_watcher_(dispatcher(), services()) {}

  void SetUp() override {
    info_context_ =
        std::make_shared<InfoContext>(&InspectRoot(), &clock_, dispatcher(), services());

    SetUpCobaltServer(std::make_unique<stubs::CobaltLoggerFactory>());
    SetUpNetworkReachabilityProvider();
    RunLoopUntilIdle();
  }

  void TearDown() override { ASSERT_TRUE(files::DeletePath(kStorePath, /*recursive=*/true)); }

 protected:
  void SetUpNetworkReachabilityProvider() {
    network_reachability_provider_ = std::make_unique<stubs::NetworkReachabilityProvider>();
    InjectServiceProvider(network_reachability_provider_.get());
  }

  void SetUpQueue(std::vector<CrashServer::UploadStatus> upload_attempt_results =
                      std::vector<CrashServer::UploadStatus>{}) {
    report_id_ = 1;
    state_ = QueueOps::SetStateToLeaveAsPending;
    expected_queue_contents_.clear();
    upload_attempt_results_ = upload_attempt_results;
    next_upload_attempt_result_ = upload_attempt_results_.cbegin();
    snapshot_manager_ = std::make_unique<SnapshotManager>(
        dispatcher(), services(), std::make_unique<timekeeper::TestClock>(), zx::sec(5),
        StorageSize::Gigabytes(1), StorageSize::Gigabytes(1));
    crash_server_ = std::make_unique<StubCrashServer>(upload_attempt_results_);
    crash_server_->AddSnapshotManager(snapshot_manager_.get());

    queue_ = std::make_unique<Queue>(dispatcher(), services(), info_context_, &tags_,
                                     crash_server_.get(), snapshot_manager_.get());
    ASSERT_TRUE(queue_);
    queue_->WatchReportingPolicy(&reporting_policy_watcher_);
    queue_->WatchNetwork(&network_watcher_);
  }

  enum class QueueOps {
    AddNewReport,
    DeleteOneReport,
    SetStateToArchive,
    SetStateToUpload,
    SetStateToLeaveAsPending,
  };

  void ApplyQueueOps(const std::vector<QueueOps>& ops) {
    for (auto const& op : ops) {
      switch (op) {
        case QueueOps::AddNewReport:
          FX_CHECK(queue_->Add(MakeReport(report_id_)));
          RunLoopUntilIdle();
          ++report_id_;
          if (!queue_->IsEmpty()) {
            AddExpectedReport(queue_->LatestReport());
          }
          SetExpectedQueueContents();
          break;
        case QueueOps::DeleteOneReport:
          if (!expected_queue_contents_.empty()) {
            std::optional<std::string> report_id = DeleteReportFromStore();
            if (report_id.has_value()) {
              expected_queue_contents_.erase(
                  std::remove(expected_queue_contents_.begin(), expected_queue_contents_.end(),
                              std::stoull(report_id.value())),
                  expected_queue_contents_.end());
            }
          }
          SetExpectedQueueContents();
          break;
        case QueueOps::SetStateToArchive:
          state_ = QueueOps::SetStateToArchive;
          reporting_policy_watcher_.Set(ReportingPolicy::kArchive);
          SetExpectedQueueContents();
          break;
        case QueueOps::SetStateToUpload:
          state_ = QueueOps::SetStateToUpload;
          reporting_policy_watcher_.Set(ReportingPolicy::kUpload);
          SetExpectedQueueContents();
          break;
        case QueueOps::SetStateToLeaveAsPending:
          state_ = QueueOps::SetStateToLeaveAsPending;
          reporting_policy_watcher_.Set(ReportingPolicy::kUndecided);
          SetExpectedQueueContents();
          break;
      }
    }
  }

  void CheckQueueContents() {
    for (const auto& id : expected_queue_contents_) {
      EXPECT_TRUE(queue_->Contains(id));
    }
    EXPECT_EQ(queue_->Size(), expected_queue_contents_.size());
  }

  void CheckAnnotationsOnServer() {
    FX_CHECK(crash_server_);

    // Expect annotations that |snapshot_manager_| will for using |kSnapshotUuidValue| as the
    // snapshot uuid.
    EXPECT_THAT(crash_server_->latest_annotations(),
                UnorderedElementsAreArray({
                    Pair(kAnnotationKey, kAnnotationValue),
                    Pair("debug.snapshot.error", "garbage collected"),
                    Pair("debug.snapshot.present", "false"),
                }));
  }

  void CheckAttachmentKeysOnServer() {
    FX_CHECK(crash_server_);
    EXPECT_THAT(crash_server_->latest_attachment_keys(),
                UnorderedElementsAre(kAttachmentKey, kMinidumpKey));
  }

  LogTags tags_;
  std::unique_ptr<Queue> queue_;
  std::vector<ReportId> expected_queue_contents_;
  std::unique_ptr<stubs::NetworkReachabilityProvider> network_reachability_provider_;

 private:
  void AddExpectedReport(const ReportId& uuid) {
    // Add a report to the back of the expected queue contents if and only if it is expected
    // to be in the queue after processing.
    if (state_ != QueueOps::SetStateToUpload) {
      expected_queue_contents_.push_back(uuid);
    } else if (*next_upload_attempt_result_ == CrashServer::UploadStatus::kFailure) {
      expected_queue_contents_.push_back(uuid);
      ++next_upload_attempt_result_;
    }
  }

  size_t SetExpectedQueueContents() {
    if (state_ == QueueOps::SetStateToArchive) {
      const size_t old_size = expected_queue_contents_.size();
      expected_queue_contents_.clear();
      return old_size;
    } else if (state_ == QueueOps::SetStateToUpload) {
      std::vector<ReportId> new_queue_contents;
      for (const auto& uuid : expected_queue_contents_) {
        // We expect the reports we failed to upload to still be pending.
        if (*next_upload_attempt_result_ == CrashServer::UploadStatus::kFailure) {
          new_queue_contents.push_back(uuid);
        }
        ++next_upload_attempt_result_;
      }
      expected_queue_contents_.swap(new_queue_contents);
      return new_queue_contents.size() - expected_queue_contents_.size();
    }
    return 0;
  }

  size_t report_id_ = 1;
  QueueOps state_ = QueueOps::SetStateToLeaveAsPending;
  std::vector<CrashServer::UploadStatus> upload_attempt_results_;
  std::vector<CrashServer::UploadStatus>::const_iterator next_upload_attempt_result_;

  TestReportingPolicyWatcher reporting_policy_watcher_;
  NetworkWatcher network_watcher_;
  timekeeper::TestClock clock_;
  std::unique_ptr<SnapshotManager> snapshot_manager_;
  std::unique_ptr<StubCrashServer> crash_server_;
  std::shared_ptr<InfoContext> info_context_;
  std::shared_ptr<cobalt::Logger> cobalt_;
};

TEST_F(QueueTest, Check_EmptyQueue_OnZeroAdds) {
  SetUpQueue();
  CheckQueueContents();
  EXPECT_TRUE(queue_->IsEmpty());
}

TEST_F(QueueTest, Check_NotIsEmptyQueue_OnStateSetToLeaveAsPending_MultipleReports) {
  SetUpQueue();
  ApplyQueueOps({
      QueueOps::AddNewReport,
      QueueOps::AddNewReport,
      QueueOps::AddNewReport,
      QueueOps::AddNewReport,
      QueueOps::AddNewReport,
  });
  CheckQueueContents();
  EXPECT_EQ(queue_->Size(), 5u);
}

TEST_F(QueueTest, Check_IsEmptyQueue_OnStateSetToArchive_MultipleReports) {
  SetUpQueue();
  ApplyQueueOps({
      QueueOps::SetStateToArchive,
      QueueOps::AddNewReport,
      QueueOps::AddNewReport,
  });
  CheckQueueContents();
  EXPECT_TRUE(queue_->IsEmpty());
}

TEST_F(QueueTest, Check_IsEmptyQueue_OnStateSetToArchive_MultipleReports_OneGarbageCollected) {
  SetUpQueue();
  ApplyQueueOps({
      QueueOps::AddNewReport,
      QueueOps::AddNewReport,
      QueueOps::AddNewReport,
      QueueOps::DeleteOneReport,
      QueueOps::SetStateToArchive,
  });
  CheckQueueContents();
  EXPECT_TRUE(queue_->IsEmpty());
}

TEST_F(QueueTest, Check_EarlyUploadSucceeds) {
  SetUpQueue({kUploadSuccessful});
  ApplyQueueOps({
      QueueOps::SetStateToUpload,
      QueueOps::AddNewReport,
  });
  CheckQueueContents();
  CheckAnnotationsOnServer();
  CheckAttachmentKeysOnServer();
  EXPECT_TRUE(queue_->IsEmpty());

  RunLoopUntilIdle();
  EXPECT_THAT(ReceivedCobaltEvents(),
              UnorderedElementsAreArray({
                  cobalt::Event(cobalt::CrashState::kUploaded),
                  cobalt::Event(cobalt::UploadAttemptState::kUploadAttempt, 1u),
                  cobalt::Event(cobalt::UploadAttemptState::kUploaded, 1u),
              }));
}

TEST_F(QueueTest, Check_EarlyUploadThrottled) {
  SetUpQueue({kUploadThrottled});
  ApplyQueueOps({
      QueueOps::SetStateToUpload,
      QueueOps::AddNewReport,
  });
  CheckQueueContents();
  CheckAnnotationsOnServer();
  CheckAttachmentKeysOnServer();
  EXPECT_TRUE(queue_->IsEmpty());

  RunLoopUntilIdle();
  EXPECT_THAT(ReceivedCobaltEvents(),
              UnorderedElementsAreArray({
                  cobalt::Event(cobalt::CrashState::kUploadThrottled),
                  cobalt::Event(cobalt::UploadAttemptState::kUploadAttempt, 1u),
                  cobalt::Event(cobalt::UploadAttemptState::kUploadThrottled, 1u),
              }));
}

TEST_F(QueueTest, Check_ThrottledReportDropped) {
  SetUpQueue({kUploadThrottled});
  ApplyQueueOps({
      QueueOps::AddNewReport,
      QueueOps::SetStateToUpload,
  });
  CheckQueueContents();
  CheckAnnotationsOnServer();
  CheckAttachmentKeysOnServer();
  EXPECT_TRUE(queue_->IsEmpty());

  RunLoopUntilIdle();
  EXPECT_THAT(ReceivedCobaltEvents(),
              UnorderedElementsAreArray({
                  cobalt::Event(cobalt::CrashState::kUploadThrottled),
                  cobalt::Event(cobalt::UploadAttemptState::kUploadAttempt, 1u),
                  cobalt::Event(cobalt::UploadAttemptState::kUploadThrottled, 1u),
              }));
}

TEST_F(QueueTest, Check_IsEmptyQueue_OnSuccessfulUpload_MultipleReports) {
  SetUpQueue(std::vector<CrashServer::UploadStatus>(5u, kUploadSuccessful));
  ApplyQueueOps({
      QueueOps::AddNewReport,
      QueueOps::AddNewReport,
      QueueOps::AddNewReport,
      QueueOps::AddNewReport,
      QueueOps::AddNewReport,
      QueueOps::SetStateToUpload,
  });
  CheckQueueContents();
  CheckAnnotationsOnServer();
  CheckAttachmentKeysOnServer();
  EXPECT_TRUE(queue_->IsEmpty());
}

TEST_F(QueueTest, Check_NotIsEmptyQueue_OnFailedUpload_MultipleReports) {
  SetUpQueue(std::vector<CrashServer::UploadStatus>(5u, kUploadFailed));
  ApplyQueueOps({
      QueueOps::AddNewReport,
      QueueOps::AddNewReport,
      QueueOps::AddNewReport,
      QueueOps::AddNewReport,
      QueueOps::AddNewReport,
      QueueOps::SetStateToUpload,
  });
  CheckQueueContents();
  CheckAnnotationsOnServer();
  CheckAttachmentKeysOnServer();
  EXPECT_FALSE(queue_->IsEmpty());
}

TEST_F(QueueTest, Check_IsEmptyQueue_OnSuccessfulUpload_OneGarbageCollected) {
  SetUpQueue({kUploadSuccessful});
  ApplyQueueOps({
      QueueOps::AddNewReport,
      QueueOps::DeleteOneReport,
      QueueOps::SetStateToUpload,
      QueueOps::AddNewReport,
  });
  CheckQueueContents();
  CheckAnnotationsOnServer();
  CheckAttachmentKeysOnServer();
  EXPECT_TRUE(queue_->IsEmpty());
}

TEST_F(QueueTest, Check_IsEmptyQueue_OnSuccessfulUpload_MultipleGarbageCollected_MultipleReports) {
  SetUpQueue({kUploadSuccessful});
  ApplyQueueOps({
      QueueOps::AddNewReport,
      QueueOps::DeleteOneReport,
      QueueOps::AddNewReport,
      QueueOps::DeleteOneReport,
      QueueOps::AddNewReport,
      QueueOps::DeleteOneReport,
      QueueOps::AddNewReport,
      QueueOps::DeleteOneReport,
      QueueOps::AddNewReport,
      QueueOps::DeleteOneReport,
      QueueOps::AddNewReport,
      QueueOps::SetStateToUpload,
  });
  CheckQueueContents();
  CheckAnnotationsOnServer();
  CheckAttachmentKeysOnServer();
  EXPECT_TRUE(queue_->IsEmpty());
}

TEST_F(QueueTest, Check_NotIsEmptyQueue_OnMixedUploadResults_MultipleReports) {
  SetUpQueue({
      kUploadSuccessful,
      kUploadSuccessful,
      kUploadFailed,
      kUploadFailed,
      kUploadSuccessful,
  });
  ApplyQueueOps({
      QueueOps::AddNewReport,
      QueueOps::AddNewReport,
      QueueOps::AddNewReport,
      QueueOps::AddNewReport,
      QueueOps::AddNewReport,
      QueueOps::SetStateToUpload,
  });
  CheckQueueContents();
  CheckAnnotationsOnServer();
  CheckAttachmentKeysOnServer();
  EXPECT_EQ(queue_->Size(), 2u);
}

TEST_F(QueueTest,
       Check_NotIsEmptyQueue_OnMixedUploadResults_MultipleGarbageCollected_MultipleReports) {
  SetUpQueue({
      kUploadSuccessful,
      kUploadSuccessful,
      kUploadFailed,
      kUploadFailed,
      kUploadSuccessful,
  });
  ApplyQueueOps({
      QueueOps::AddNewReport,
      QueueOps::AddNewReport,
      QueueOps::AddNewReport,
      QueueOps::AddNewReport,
      QueueOps::AddNewReport,
      QueueOps::AddNewReport,
      QueueOps::AddNewReport,
      QueueOps::DeleteOneReport,
      QueueOps::DeleteOneReport,
      QueueOps::SetStateToUpload,
  });
  CheckQueueContents();
  CheckAnnotationsOnServer();
  CheckAttachmentKeysOnServer();
  EXPECT_EQ(queue_->Size(), 2u);
}

TEST_F(QueueTest, Check_UploadAll_CancelledAndPosted) {
  SetUpQueue({
      kUploadFailed,
      kUploadSuccessful,
      kUploadFailed,
  });

  // The upload task shouldn't run.
  ApplyQueueOps({
      QueueOps::SetStateToLeaveAsPending,
      QueueOps::AddNewReport,
  });
  RunLoopFor(kPeriodicUploadDuration);
  EXPECT_FALSE(queue_->IsEmpty());

  // The upload task should upload the report.
  ApplyQueueOps({
      QueueOps::SetStateToUpload,
  });
  RunLoopFor(kPeriodicUploadDuration);
  EXPECT_TRUE(queue_->IsEmpty());

  // The state change should cancel the upload task.
  ApplyQueueOps({
      QueueOps::SetStateToLeaveAsPending,
      QueueOps::AddNewReport,
  });
  RunLoopFor(kPeriodicUploadDuration);
  EXPECT_FALSE(queue_->IsEmpty());

  // The state change should cancel the upload task.
  ApplyQueueOps({
      QueueOps::SetStateToUpload,
      QueueOps::SetStateToArchive,
      QueueOps::AddNewReport,
  });
  RunLoopFor(kPeriodicUploadDuration);
  EXPECT_TRUE(queue_->IsEmpty());
}

TEST_F(QueueTest, Check_UploadAll_ScheduledTwice) {
  SetUpQueue({
      kUploadFailed,
      kUploadSuccessful,
      kUploadFailed,
      kUploadSuccessful,
  });
  ApplyQueueOps({
      QueueOps::AddNewReport,
      QueueOps::SetStateToUpload,
  });
  ASSERT_FALSE(queue_->IsEmpty());

  RunLoopFor(kPeriodicUploadDuration);
  EXPECT_TRUE(queue_->IsEmpty());

  ApplyQueueOps({
      QueueOps::SetStateToLeaveAsPending,
      QueueOps::AddNewReport,
      QueueOps::SetStateToUpload,
  });
  ASSERT_FALSE(queue_->IsEmpty());

  RunLoopFor(kPeriodicUploadDuration);
  EXPECT_TRUE(queue_->IsEmpty());
}

TEST_F(QueueTest, Check_UploadAllTwice_OnNetworkReachable) {
  // Setup crash report upload outcome
  SetUpQueue({
      // First crash report: automatic upload fails (no early upload as upload not enabled at
      // first), succeed when the network becomes reachable.
      kUploadFailed,
      kUploadSuccessful,
      // Second crash report: automatic upload fails (no early upload as upload not enabled at
      // first), succeed when then network becomes reachable.
      kUploadFailed,
      kUploadSuccessful,
  });

  // First crash report: automatic upload fails. Succeed on the second upload attempt when the
  // network becomes reachable.
  ApplyQueueOps({
      QueueOps::AddNewReport,
      QueueOps::SetStateToUpload,
  });
  ASSERT_FALSE(queue_->IsEmpty());
  network_reachability_provider_->TriggerOnNetworkReachable(true);
  RunLoopUntilIdle();
  EXPECT_TRUE(queue_->IsEmpty());

  // Second crash report: Insert a new crash report that fails to upload at first,
  // and then check that it gets uploaded when the network becomes reachable again.
  ApplyQueueOps({
      QueueOps::SetStateToLeaveAsPending,
      QueueOps::AddNewReport,
      QueueOps::SetStateToUpload,
  });
  ASSERT_FALSE(queue_->IsEmpty());
  network_reachability_provider_->TriggerOnNetworkReachable(false);
  RunLoopUntilIdle();
  EXPECT_FALSE(queue_->IsEmpty());
  network_reachability_provider_->TriggerOnNetworkReachable(true);
  RunLoopUntilIdle();
  EXPECT_TRUE(queue_->IsEmpty());
}

TEST_F(QueueTest, Check_UploadAll_OnReconnect_NetworkReachable) {
  // Setup crash report upload outcome: Automatic upload fails,
  // succeed when the network becomes reachable
  SetUpQueue({
      kUploadFailed,
      kUploadSuccessful,
  });

  // Automatic crash report upload fails.
  ApplyQueueOps({
      QueueOps::AddNewReport,
      QueueOps::SetStateToUpload,
  });
  ASSERT_FALSE(queue_->IsEmpty());

  // Close the connection to the network reachability service.
  network_reachability_provider_->CloseConnection();

  // We run the loop longer than the delay to account for the nondeterminism of
  // backoff::ExponentialBackoff.
  RunLoopFor(zx::min(3));

  // We should be re-connected to the network reachability service.
  // Test upload on network reachable.
  network_reachability_provider_->TriggerOnNetworkReachable(false);
  RunLoopUntilIdle();
  EXPECT_FALSE(queue_->IsEmpty());
  network_reachability_provider_->TriggerOnNetworkReachable(true);
  RunLoopUntilIdle();
  EXPECT_TRUE(queue_->IsEmpty());
}

TEST_F(QueueTest, Check_Skip_UploadAll_StateIsLeaveAsPending) {
  SetUpQueue();

  // Automatic crash report upload fails.
  ApplyQueueOps({
      QueueOps::SetStateToLeaveAsPending,
      QueueOps::AddNewReport,
  });
  ASSERT_FALSE(queue_->IsEmpty());

  // The periodic upload task shouldn't cause reports to be uploaded.
  RunLoopFor(kPeriodicUploadDuration);
  EXPECT_FALSE(queue_->IsEmpty());

  // The network becoming reachable shouldn't cause reports to be uploaded.
  network_reachability_provider_->TriggerOnNetworkReachable(true);
  RunLoopUntilIdle();
  EXPECT_FALSE(queue_->IsEmpty());
}

TEST_F(QueueTest, Check_Cobalt) {
  SetUpQueue({
      kUploadSuccessful,
      kUploadSuccessful,
      kUploadFailed,
      kUploadFailed,
      kUploadSuccessful,
      kUploadThrottled,
  });
  ApplyQueueOps({
      QueueOps::AddNewReport,
      QueueOps::AddNewReport,
      QueueOps::AddNewReport,
      QueueOps::AddNewReport,
      QueueOps::AddNewReport,
      QueueOps::AddNewReport,
      QueueOps::SetStateToUpload,
      QueueOps::SetStateToArchive,
  });

  RunLoopUntilIdle();
  EXPECT_THAT(ReceivedCobaltEvents(),
              UnorderedElementsAreArray({
                  cobalt::Event(cobalt::CrashState::kUploaded),
                  cobalt::Event(cobalt::CrashState::kUploaded),
                  cobalt::Event(cobalt::CrashState::kUploaded),
                  cobalt::Event(cobalt::CrashState::kUploadThrottled),
                  cobalt::Event(cobalt::CrashState::kArchived),
                  cobalt::Event(cobalt::CrashState::kArchived),
                  cobalt::Event(cobalt::UploadAttemptState::kUploadAttempt, 1u),
                  cobalt::Event(cobalt::UploadAttemptState::kUploaded, 1u),
                  cobalt::Event(cobalt::UploadAttemptState::kUploadAttempt, 1u),
                  cobalt::Event(cobalt::UploadAttemptState::kUploaded, 1u),
                  cobalt::Event(cobalt::UploadAttemptState::kUploadAttempt, 1u),
                  cobalt::Event(cobalt::UploadAttemptState::kUploaded, 1u),
                  cobalt::Event(cobalt::UploadAttemptState::kUploadAttempt, 1u),
                  cobalt::Event(cobalt::UploadAttemptState::kUploadThrottled, 1u),
                  cobalt::Event(cobalt::UploadAttemptState::kUploadAttempt, 1u),
                  cobalt::Event(cobalt::UploadAttemptState::kArchived, 1u),
                  cobalt::Event(cobalt::UploadAttemptState::kUploadAttempt, 1u),
                  cobalt::Event(cobalt::UploadAttemptState::kArchived, 1u),
              }));
}

TEST_F(QueueTest, Check_CobaltMultipleUploadAttempts) {
  SetUpQueue({
      kUploadFailed,
      kUploadSuccessful,
      kUploadSuccessful,
  });
  ApplyQueueOps({
      QueueOps::SetStateToUpload,
      QueueOps::AddNewReport,
      QueueOps::AddNewReport,
  });

  RunLoopFor(kPeriodicUploadDuration);
  EXPECT_THAT(ReceivedCobaltEvents(),
              UnorderedElementsAreArray({
                  // Two reports were eventually uploaded.
                  cobalt::Event(cobalt::CrashState::kUploaded),
                  cobalt::Event(cobalt::CrashState::kUploaded),
                  // The first report required two tries.
                  cobalt::Event(cobalt::UploadAttemptState::kUploadAttempt, 1u),
                  cobalt::Event(cobalt::UploadAttemptState::kUploadAttempt, 2u),
                  cobalt::Event(cobalt::UploadAttemptState::kUploaded, 2u),
                  // The second report only needed one try.
                  cobalt::Event(cobalt::UploadAttemptState::kUploadAttempt, 1u),
                  cobalt::Event(cobalt::UploadAttemptState::kUploaded, 1u),
              }));
}

}  // namespace
}  // namespace crash_reports
}  // namespace forensics
