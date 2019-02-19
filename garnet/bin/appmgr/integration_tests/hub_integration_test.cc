// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/sys/cpp/fidl.h>
#include <lib/async/default.h>

#include "garnet/bin/sysmgr/config.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "lib/component/cpp/testing/test_util.h"
#include "lib/component/cpp/testing/test_with_environment.h"
#include "lib/fxl/files/directory.h"
#include "lib/fxl/files/file.h"
#include "lib/fxl/files/glob.h"
#include "lib/fxl/strings/concatenate.h"
#include "lib/fxl/strings/join_strings.h"
#include "lib/fxl/strings/string_printf.h"
#include "lib/svc/cpp/services.h"
#include "lib/sys/cpp/file_descriptor.h"

namespace component {
namespace {

// This test fixture will provide a way to run components in provided launchers
// and check for errors.
class HubTest : public component::testing::TestWithEnvironment {
 protected:
  // This would launch component and check that it returns correct
  // |expected_return_code|.
  void RunComponent(const fuchsia::sys::LauncherPtr& launcher,
                    const std::string& component_url,
                    const std::vector<std::string>& args,
                    int64_t expected_return_code) {
    std::FILE* outf = std::tmpfile();
    int out_fd = fileno(outf);
    fuchsia::sys::LaunchInfo launch_info;
    launch_info.url = component_url;
    for (auto arg : args) {
      launch_info.arguments.push_back(arg);
    }

    launch_info.out = sys::CloneFileDescriptor(out_fd);

    fuchsia::sys::ComponentControllerPtr controller;
    launcher->CreateComponent(std::move(launch_info), controller.NewRequest());

    int64_t return_code = INT64_MIN;
    controller.events().OnTerminated =
        [&return_code](int64_t code, fuchsia::sys::TerminationReason reason) {
          return_code = code;
        };
    ASSERT_TRUE(RunLoopWithTimeoutOrUntil(
        [&return_code] { return return_code != INT64_MIN; }, zx::sec(10)));
    std::string output;
    ASSERT_TRUE(files::ReadFileDescriptorToString(out_fd, &output));
    EXPECT_EQ(expected_return_code, return_code)
        << "failed for: " << fxl::JoinStrings(args, ", ")
        << "\noutput: " << output;
  }
};

TEST(ProbeHub, Component) {
  constexpr char kGlob[] = "/hub/c/*/*/out/debug";
  files::Glob glob(kGlob);
  EXPECT_GE(glob.size(), 1u) << kGlob << " expected to match at least once.";
}

TEST(ProbeHub, Realm) {
  constexpr char kGlob[] = "/hub/c/";
  files::Glob glob(kGlob);
  EXPECT_EQ(glob.size(), 1u) << kGlob << " expected to match once.";
}

TEST(ProbeHub, RealmSvc) {
  constexpr char kGlob[] = "/hub/svc/fuchsia.sys.Environment";
  files::Glob glob(kGlob);
  EXPECT_EQ(glob.size(), 1u);
}

TEST_F(HubTest, Services) {
  // Services for sys.
  {
    constexpr char kGlob[] = "/hub/svc";
    files::Glob glob(kGlob);
    EXPECT_EQ(glob.size(), 1u) << kGlob << " expected to match once.";
    const std::string path = *glob.begin();

    // Expected files are built-in services plus sysmgr services.
    std::vector<std::string> expected_files = {
        ".",
        "fuchsia.process.Launcher",
        "fuchsia.process.Resolver",
        "fuchsia.scheduler.ProfileProvider",
        "fuchsia.sys.Environment",
        "fuchsia.sys.Launcher",
        "fuchsia.sys.Loader"};
    sysmgr::Config config;
    ASSERT_TRUE(config.ParseFromDirectory("/system/data/sysmgr"));
    const auto service_map = config.TakeServices();
    for (const auto& e : service_map) {
      expected_files.push_back(e.first);
    }

    // readdir should list all services.
    std::vector<std::string> files;
    ASSERT_TRUE(files::ReadDirContents(path, &files));
    EXPECT_THAT(files, ::testing::UnorderedElementsAreArray(expected_files));

    // Try looking up an individual service.
    const std::string service_path =
        fxl::Concatenate({path, "/", service_map.begin()->first});
    EXPECT_TRUE(files::IsFile(service_path)) << service_path;
    const std::string bogus_path = fxl::Concatenate({path, "/does_not_exist"});
    EXPECT_FALSE(files::IsFile(bogus_path)) << bogus_path;
  }
}

TEST_F(HubTest, ScopePolicy) {
  constexpr char kGlobUrl[] = "fuchsia-pkg://fuchsia.com/glob#meta/glob.cmx";
  // create nested environment
  // test that we can see nested env
  auto nested_env =
      CreateNewEnclosingEnvironment("hubscopepolicytest", CreateServices());
  ASSERT_TRUE(WaitForEnclosingEnvToStart(nested_env.get()));
  RunComponent(launcher_ptr(), kGlobUrl, {"/hub/r/hubscopepolicytest/"}, 0);

  // test that we cannot see nested env using its own launcher
  RunComponent(nested_env->launcher_ptr(), kGlobUrl,
               {"/hub/r/hubscopepolicytest"}, 1);

  // test that we can see check_hub_path
  RunComponent(nested_env->launcher_ptr(), kGlobUrl, {"/hub/c/glob.cmx"}, 0);
}

TEST_F(HubTest, SystemObjects) {
  std::string glob_url = "fuchsia-pkg://fuchsia.com/glob#meta/glob.cmx";

  auto nested_env =
      CreateNewEnclosingEnvironment("hubscopepolicytest", CreateServices());
  ASSERT_TRUE(WaitForEnclosingEnvToStart(nested_env.get()));
  RunComponent(launcher_ptr(), glob_url, {"/hub/r/hubscopepolicytest/"}, 0);

  // test that we can see system objects
  RunComponent(nested_env->launcher_ptr(), glob_url,
               {"/hub/c/glob.cmx/*/system_objects"}, 0);
}

}  // namespace
}  // namespace component
