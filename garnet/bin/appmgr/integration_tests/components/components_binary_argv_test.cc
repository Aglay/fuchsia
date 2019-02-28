// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/sys/cpp/file_descriptor.h>
#include "lib/component/cpp/testing/test_with_environment.h"
#include "src/lib/files/file.h"
#include "src/lib/files/scoped_temp_dir.h"

namespace component {
namespace {

using testing::EnclosingEnvironment;

constexpr char kTestComponent1[] =
    "fuchsia-pkg://fuchsia.com/components_binary_test#meta/program1.cmx";
constexpr char kTestComponent2[] =
    "fuchsia-pkg://fuchsia.com/components_binary_test#meta/program2.cmx";
constexpr char kRealm[] = "test";

class ComponentsBinaryArgvTest : public component::testing::TestWithEnvironment {
 protected:
  void OpenNewOutFile() {
    ASSERT_TRUE(tmp_dir_.NewTempFile(&out_file_));
    outf_ = fileno(std::fopen(out_file_.c_str(), "w"));
  }

  std::string ReadOutFile() {
    std::string out;
    if (!files::ReadFileToString(out_file_, &out)) {
      FXL_LOG(ERROR) << "Could not read output file " << out_file_;
      return "";
    }
    return out;
  }

  fuchsia::sys::LaunchInfo CreateLaunchInfo(
      const std::string& url, const std::vector<std::string>& args = {}) {
    fuchsia::sys::LaunchInfo launch_info;
    launch_info.url = url;
    for (const auto& a : args) {
      launch_info.arguments.push_back(a);
    }
    launch_info.out = sys::CloneFileDescriptor(outf_);
    launch_info.err = sys::CloneFileDescriptor(STDERR_FILENO);
    return launch_info;
  }

  void RunComponent(const std::string& url, const std::vector<std::string>& args = {}) {
    fuchsia::sys::ComponentControllerPtr controller;
    environment_->CreateComponent(CreateLaunchInfo(url, std::move(args)), controller.NewRequest());

    int64_t return_code = INT64_MIN;
    controller.events().OnTerminated =
        [&return_code](int64_t code, fuchsia::sys::TerminationReason reason) {
          return_code = code;
        };
    ASSERT_TRUE(
        RunLoopUntil([&return_code] { return return_code != INT64_MIN; }));
    EXPECT_EQ(0, return_code);
  }

  ComponentsBinaryArgvTest() {
    OpenNewOutFile();

    environment_ = CreateNewEnclosingEnvironment(kRealm, CreateServices());
  }

 private:
  std::unique_ptr<EnclosingEnvironment> environment_;
  files::ScopedTempDir tmp_dir_;
  std::string out_file_;
  int outf_;
};

// We therefore test that targeting a binary by a component manifest works, that
// argv0 properly propagates the binary path, and that the args field in the
// manifest is being properly passed through to the component.
TEST_F(ComponentsBinaryArgvTest, Foo) {
  RunComponent(kTestComponent1);
  std::string output = ReadOutFile();
  ASSERT_EQ(output, "/pkg/bin/app\n");
}

TEST_F(ComponentsBinaryArgvTest, Bar) {
  RunComponent(kTestComponent2);
  std::string output = ReadOutFile();
  ASSERT_EQ(output, "/pkg/bin/app2 helloworld\n");
}

}  // namespace
}  // namespace component
