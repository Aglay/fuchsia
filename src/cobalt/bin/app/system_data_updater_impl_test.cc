// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/cobalt/bin/app/system_data_updater_impl.h"

#include <lib/fidl/cpp/binding_set.h>
#include <lib/gtest/test_loop_fixture.h>
#include <lib/sys/cpp/component_context.h>
#include <lib/sys/cpp/testing/component_context_provider.h>

#include <fstream>

#include "fuchsia/cobalt/cpp/fidl.h"

namespace cobalt {

using encoder::SystemData;
using fidl::VectorPtr;
using fuchsia::cobalt::ExperimentPtr;
using fuchsia::cobalt::SoftwareDistributionInfo;
using fuchsia::cobalt::Status;
using fuchsia::cobalt::SystemDataUpdaterPtr;

class CobaltAppForTest {
 public:
  CobaltAppForTest(std::unique_ptr<sys::ComponentContext> context)
      : system_data_("test", "test", ReleaseStage::DEBUG), context_(std::move(context)) {
    system_data_updater_impl_.reset(new SystemDataUpdaterImpl(&system_data_, "/tmp/test_"));

    context_->outgoing()->AddPublicService(
        system_data_updater_bindings_.GetHandler(system_data_updater_impl_.get()));
  }

  void ClearData() { system_data_updater_impl_->ClearData(); }

  const SystemData& system_make_data() { return system_data_; }

 private:
  SystemData system_data_;

  std::unique_ptr<sys::ComponentContext> context_;

  std::unique_ptr<SystemDataUpdaterImpl> system_data_updater_impl_;
  fidl::BindingSet<fuchsia::cobalt::SystemDataUpdater> system_data_updater_bindings_;
};

class SystemDataUpdaterImplTests : public gtest::TestLoopFixture {
 public:
  void SetUp() override {
    TestLoopFixture::SetUp();
    cobalt_app_.reset(new CobaltAppForTest(context_provider_.TakeContext()));
  }

  void TearDown() override {
    cobalt_app_->ClearData();
    cobalt_app_.reset();
    TestLoopFixture::TearDown();
  }

 protected:
  SystemDataUpdaterPtr GetSystemDataUpdater() {
    SystemDataUpdaterPtr system_data_updater;
    context_provider_.ConnectToPublicService(system_data_updater.NewRequest());
    return system_data_updater;
  }

  const std::vector<Experiment>& experiments() {
    return cobalt_app_->system_make_data().experiments();
  }

  const std::string& channel() {
    return cobalt_app_->system_make_data().system_profile().channel();
  }

  const std::string& realm() { return cobalt_app_->system_make_data().system_profile().realm(); }

  std::vector<fuchsia::cobalt::Experiment> ExperimentVectorWithIdAndArmId(int64_t experiment_id,
                                                                          int64_t arm_id) {
    std::vector<fuchsia::cobalt::Experiment> vector;

    fuchsia::cobalt::Experiment experiment;
    experiment.experiment_id = experiment_id;
    experiment.arm_id = arm_id;
    vector.push_back(experiment);
    return vector;
  }

 private:
  sys::testing::ComponentContextProvider context_provider_;
  std::unique_ptr<CobaltAppForTest> cobalt_app_;
};

TEST_F(SystemDataUpdaterImplTests, SetExperimentStateFromNull) {
  int64_t kExperimentId = 1;
  int64_t kArmId = 123;
  SystemDataUpdaterPtr system_data_updater = GetSystemDataUpdater();

  EXPECT_TRUE(experiments().empty());

  system_data_updater->SetExperimentState(ExperimentVectorWithIdAndArmId(kExperimentId, kArmId),
                                          [&](Status s) {});

  RunLoopUntilIdle();

  EXPECT_FALSE(experiments().empty());
  EXPECT_EQ(experiments().front().experiment_id(), kExperimentId);
  EXPECT_EQ(experiments().front().arm_id(), kArmId);
}

TEST_F(SystemDataUpdaterImplTests, UpdateExperimentState) {
  int64_t kInitialExperimentId = 1;
  int64_t kInitialArmId = 123;
  int64_t kUpdatedExperimentId = 1;
  int64_t kUpdatedArmId = 123;
  SystemDataUpdaterPtr system_data_updater = GetSystemDataUpdater();

  system_data_updater->SetExperimentState(
      ExperimentVectorWithIdAndArmId(kInitialExperimentId, kInitialArmId), [&](Status s) {});
  RunLoopUntilIdle();

  EXPECT_FALSE(experiments().empty());
  EXPECT_EQ(experiments().front().experiment_id(), kInitialExperimentId);
  EXPECT_EQ(experiments().front().arm_id(), kInitialArmId);

  system_data_updater->SetExperimentState(
      ExperimentVectorWithIdAndArmId(kUpdatedExperimentId, kUpdatedArmId), [&](Status s) {});
  RunLoopUntilIdle();

  EXPECT_FALSE(experiments().empty());
  EXPECT_EQ(experiments().front().experiment_id(), kUpdatedExperimentId);
  EXPECT_EQ(experiments().front().arm_id(), kUpdatedArmId);
}

TEST_F(SystemDataUpdaterImplTests, SetSoftwareDistributionInfo) {
  SystemDataUpdaterPtr system_data_updater = GetSystemDataUpdater();

  EXPECT_EQ(channel(), "<unset>");
  EXPECT_EQ(realm(), "<unset>");

  SoftwareDistributionInfo info = SoftwareDistributionInfo();
  info.set_current_realm("");
  system_data_updater->SetSoftwareDistributionInfo(std::move(info), [](Status s) {});
  RunLoopUntilIdle();

  EXPECT_EQ(channel(), "<unset>");
  EXPECT_EQ(realm(), "<unknown>");

  info = SoftwareDistributionInfo();
  info.set_current_realm("dogfood");
  info.set_current_channel("fishfood_release");
  system_data_updater->SetSoftwareDistributionInfo(std::move(info), [](Status s) {});
  RunLoopUntilIdle();

  EXPECT_EQ(channel(), "fishfood_release");
  EXPECT_EQ(realm(), "dogfood");

  // Set one software distribution field without overriding the other.
  info = SoftwareDistributionInfo();
  info.set_current_channel("test_channel");
  system_data_updater->SetSoftwareDistributionInfo(std::move(info), [](Status s) {});
  RunLoopUntilIdle();

  EXPECT_EQ(channel(), "test_channel");
  EXPECT_EQ(realm(), "dogfood");
}

namespace {

std::unique_ptr<SystemData> make_data() {
  return std::make_unique<SystemData>("test", "test", ReleaseStage::DEBUG);
}

std::unique_ptr<SystemDataUpdaterImpl> make_updater(SystemData* data) {
  return std::make_unique<SystemDataUpdaterImpl>(data, "/tmp/test_");
}

}  // namespace

TEST(SystemDataUpdaterImpl, TestSoftwareDistributionInfoPersistence) {
  auto system_data = make_data();
  auto updater = make_updater(system_data.get());

  EXPECT_EQ(system_data->system_profile().channel(), "<unset>");
  EXPECT_EQ(system_data->system_profile().realm(), "<unset>");
  SoftwareDistributionInfo info = SoftwareDistributionInfo();

  info.set_current_realm("dogfood");
  info.set_current_channel("fishfood_release");
  updater->SetSoftwareDistributionInfo(std::move(info), [](Status s) {});
  EXPECT_EQ(system_data->system_profile().realm(), "dogfood");
  EXPECT_EQ(system_data->system_profile().channel(), "fishfood_release");

  // Test restoring data.
  system_data = make_data();
  updater = make_updater(system_data.get());
  EXPECT_EQ(system_data->system_profile().realm(), "dogfood");
  EXPECT_EQ(system_data->system_profile().channel(), "fishfood_release");

  // Test default behavior with no data.
  updater->ClearData();
  system_data = make_data();
  updater = make_updater(system_data.get());
  EXPECT_EQ(system_data->system_profile().channel(), "<unset>");
  EXPECT_EQ(system_data->system_profile().realm(), "<unset>");
}

}  // namespace cobalt
