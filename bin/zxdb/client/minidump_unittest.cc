// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <filesystem>

#include "garnet/bin/zxdb/client/remote_api.h"
#include "garnet/bin/zxdb/client/session.h"
#include "garnet/bin/zxdb/common/host_util.h"
#include "garnet/lib/debug_ipc/helper/platform_message_loop.h"
#include "gtest/gtest.h"

namespace zxdb {

class MinidumpTest : public testing::Test {
 public:
  MinidumpTest();
  virtual ~MinidumpTest();

  debug_ipc::PlatformMessageLoop& loop() { return loop_; }
  Session& session() { return *session_; }

  Err TryOpen(const std::string& filename);

 private:
  debug_ipc::PlatformMessageLoop loop_;
  std::unique_ptr<Session> session_;
};

MinidumpTest::MinidumpTest() {
  loop_.Init();
  session_ = std::make_unique<Session>();
}

MinidumpTest::~MinidumpTest() { loop_.Cleanup(); }

Err MinidumpTest::TryOpen(const std::string& filename) {
  static auto data_dir = std::filesystem::path(GetSelfPath())
    .parent_path().parent_path() / "test_data" / "zxdb";

  Err err;
  auto path = (data_dir / filename).string();

  session().OpenMinidump(path,
                         [&err](const Err& got) {
                           err = got;
                           debug_ipc::MessageLoop::Current()->QuitNow();
                         });

  loop().Run();

  return err;
}

TEST_F(MinidumpTest, Load) {
  Err err = TryOpen("test_example_minidump.dmp");
  EXPECT_FALSE(err.has_error()) << err.msg();
}

TEST_F(MinidumpTest, ProcessTreeRecord) {
  Err err = TryOpen("test_example_minidump.dmp");
  ASSERT_FALSE(err.has_error()) << err.msg();

  debug_ipc::ProcessTreeRecord record;
  session().remote_api()->ProcessTree(debug_ipc::ProcessTreeRequest(),
    [&record, &err](const Err& e, debug_ipc::ProcessTreeReply r) {
      err = e;
      record = r.root;
      debug_ipc::MessageLoop::Current()->QuitNow();
    }
  );

  loop().Run();
  ASSERT_FALSE(err.has_error()) << err.msg();

  EXPECT_EQ(debug_ipc::ProcessTreeRecord::Type::kProcess, record.type);
  EXPECT_EQ("<core dump>", record.name);
  EXPECT_EQ(656254UL, record.koid);
}

}  // namespace zxdb
