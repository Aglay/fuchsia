// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>

#include <gtest/gtest.h>
#include <lib/component/cpp/testing/test_with_environment.h>
#include <lib/fxl/logging.h>
#include <lib/svc/cpp/services.h>
#include <test/appmgr/integration/cpp/fidl.h>
#include <zircon/syscalls.h>

#include "garnet/bin/appmgr/integration_tests/util/data_file_reader_writer_util.h"

namespace {

constexpr char kEnvironmentLabel1[] = "test-env-1";
constexpr char kEnvironmentLabel2[] = "test-env-2";
constexpr char kTestFileName[] = "some-test-file";
// Each of these component manifests have the same content (same test util
// binary, same sandbox definition), but we have two so we can test storage
// isolation based on component URL
// Note that the test_unil manifest includes both the new isolated and old
// features to validate that the isolated feature is prioritized if both are
// included. (This is mentioned here since the manifests are JSON and can't have
// comments)
constexpr char kTestUtilURL[] =
    "fuchsia-pkg://fuchsia.com/persistent_storage_test_util#meta/util.cmx";
constexpr char kDifferentTestUtilURL[] =
    "fuchsia-pkg://fuchsia.com/persistent_storage_test_util#meta/util2.cmx";

using test::appmgr::integration::DataFileReaderWriterPtr;

class IsolatedPersistentStorageTest
    : virtual public component::testing::TestWithEnvironment,
      public component::testing::DataFileReaderWriterUtil {
 protected:
  IsolatedPersistentStorageTest()
      : TestWithEnvironment(),
        env1_(CreateNewEnclosingEnvironment(kEnvironmentLabel1,
                                            CreateServices())),
        env2_(CreateNewEnclosingEnvironment(kEnvironmentLabel2,
                                            CreateServices())) {}

  void SetUp() override {
    TestWithEnvironment::SetUp();

    // Random file contents used since we don't explicitly clear /data contents
    // between test runs, and we want to ensure we aren't reading a file written
    // by a previous run.
    char random_bytes[100];
    zx_cprng_draw(random_bytes, sizeof(random_bytes));
    test_file_content_ = std::string(random_bytes, sizeof(random_bytes));
  }

  // Verify that a file written in one component's /data dir is not accessible
  // by the other component.
  void VerifyIsolated(component::Services services1,
                      component::Services services2) {
    DataFileReaderWriterPtr util1, util2;
    services1.ConnectToService(util1.NewRequest());
    services2.ConnectToService(util2.NewRequest());

    // Can't use ASSERT_TRUE/ASSERT_EQ macros here since this isn't the test
    // body, and ASSERT_* macros just 'return;' to exit the test.
    EXPECT_EQ(WriteFileSync(util1, kTestFileName, test_file_content_), ZX_OK);
    EXPECT_EQ(ReadFileSync(util1, kTestFileName).get(), test_file_content_);
    EXPECT_NE(ReadFileSync(util2, kTestFileName).get(), test_file_content_);
  }

  std::unique_ptr<component::testing::EnclosingEnvironment> env1_;
  std::unique_ptr<component::testing::EnclosingEnvironment> env2_;
  std::string test_file_content_;
};

TEST_F(IsolatedPersistentStorageTest, SameComponentDifferentEnvironments) {
  // Create two instances of the same utility component in separate
  // environments.
  component::Services services1, services2;
  fuchsia::sys::ComponentControllerPtr ctrl1, ctrl2;
  fuchsia::sys::LaunchInfo launch_info1{
      .url = kTestUtilURL,
      .directory_request = services1.NewRequest(),
  };
  fuchsia::sys::LaunchInfo launch_info2{
      .url = kTestUtilURL,
      .directory_request = services2.NewRequest(),
  };
  env1_->CreateComponent(std::move(launch_info1), ctrl1.NewRequest());
  env2_->CreateComponent(std::move(launch_info2), ctrl2.NewRequest());

  VerifyIsolated(std::move(services1), std::move(services2));
}

TEST_F(IsolatedPersistentStorageTest, SameComponentNestedEnvironments) {
  // Create a nested environment inside the environment created by the test
  // fixture, using the same label.
  auto env1_nested =
      env1_->CreateNestedEnclosingEnvironment(kEnvironmentLabel1);

  // Create two instances of the same utility component in the parent and child
  // environments.
  component::Services services1, services2;
  fuchsia::sys::ComponentControllerPtr ctrl1, ctrl2;
  fuchsia::sys::LaunchInfo launch_info1{
      .url = kTestUtilURL,
      .directory_request = services1.NewRequest(),
  };
  fuchsia::sys::LaunchInfo launch_info2{
      .url = kTestUtilURL,
      .directory_request = services2.NewRequest(),
  };
  env1_->CreateComponent(std::move(launch_info1), ctrl1.NewRequest());
  env1_nested->CreateComponent(std::move(launch_info2), ctrl2.NewRequest());

  VerifyIsolated(std::move(services1), std::move(services2));
}

TEST_F(IsolatedPersistentStorageTest, DifferentComponentsSameEnvironment) {
  // Create two instances of the same utility component in separate
  // environments.
  component::Services services1, services2;
  fuchsia::sys::ComponentControllerPtr ctrl1, ctrl2;
  fuchsia::sys::LaunchInfo launch_info1{
      .url = kTestUtilURL,
      .directory_request = services1.NewRequest(),
  };
  fuchsia::sys::LaunchInfo launch_info2{
      .url = kDifferentTestUtilURL,
      .directory_request = services2.NewRequest(),
  };
  env1_->CreateComponent(std::move(launch_info1), ctrl1.NewRequest());
  env1_->CreateComponent(std::move(launch_info2), ctrl2.NewRequest());

  VerifyIsolated(std::move(services1), std::move(services2));
}

TEST_F(IsolatedPersistentStorageTest, SameComponentSameEnvironment) {
  // Create utility component in some environment.
  component::Services services;
  fuchsia::sys::ComponentControllerPtr ctrl;
  DataFileReaderWriterPtr util;
  fuchsia::sys::LaunchInfo launch_info{
      .url = kTestUtilURL,
      .directory_request = services.NewRequest(),
  };
  env1_->CreateComponent(std::move(launch_info), ctrl.NewRequest());
  services.ConnectToService(util.NewRequest());

  EXPECT_EQ(WriteFileSync(util, kTestFileName, test_file_content_), ZX_OK);
  EXPECT_EQ(ReadFileSync(util, kTestFileName).get(), test_file_content_);

  // Kill the component and then recreate it in the same environment.
  ctrl->Kill();
  launch_info = fuchsia::sys::LaunchInfo{
      .url = kTestUtilURL,
      .directory_request = services.NewRequest(),
  };
  env1_->CreateComponent(std::move(launch_info), ctrl.NewRequest());
  services.ConnectToService(util.NewRequest());

  // File should still exist.
  EXPECT_EQ(ReadFileSync(util, kTestFileName).get(), test_file_content_);
}

}  // namespace
