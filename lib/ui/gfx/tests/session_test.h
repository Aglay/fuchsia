// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_LIB_UI_GFX_TESTS_SESSION_TEST_H_
#define GARNET_LIB_UI_GFX_TESTS_SESSION_TEST_H_

#include "garnet/lib/ui/gfx/tests/error_reporting_test.h"

#include <lib/fit/function.h>

#include "garnet/lib/ui/gfx/displays/display_manager.h"
#include "garnet/lib/ui/gfx/engine/engine.h"
#include "garnet/lib/ui/gfx/engine/session.h"
#include "garnet/lib/ui/gfx/tests/mocks.h"
#include "garnet/lib/ui/scenic/event_reporter.h"
#include "lib/gtest/test_loop_fixture.h"

namespace scenic_impl {
namespace gfx {
namespace test {

class FakeUpdateScheduler : public UpdateScheduler {
 public:
  FakeUpdateScheduler(SessionManager* session_manager);

  void ScheduleUpdate(uint64_t presentation_time) override;

 private:
  SessionManager* session_manager_ = nullptr;
};

class SessionTest : public ErrorReportingTest, public EventReporter {
 protected:
  // | ::testing::Test |
  void SetUp() override;
  void TearDown() override;

  // |EventReporter|
  void EnqueueEvent(fuchsia::ui::gfx::Event event) override;
  void EnqueueEvent(fuchsia::ui::input::InputEvent event) override;
  void EnqueueEvent(fuchsia::ui::scenic::Command unhandled) override;

  // Subclasses should override to provide their own Session.
  virtual fxl::RefPtr<SessionForTest> CreateSession();

  // Creates a SessionContext with only a SessionManager and a
  // FakeUpdateScheduler.
  SessionContext CreateBarebonesSessionContext();

  // Apply the specified Command.  Return true if it was applied successfully,
  // and false if an error occurred.
  bool Apply(::fuchsia::ui::gfx::Command command) {
    CommandContext empty_command_context(nullptr);
    return session_->ApplyCommand(&empty_command_context, std::move(command));
  }

  template <class ResourceT>
  fxl::RefPtr<ResourceT> FindResource(ResourceId id) {
    return session_->resources()->FindResource<ResourceT>(id);
  }

  std::unique_ptr<SessionManager> session_manager_;
  std::unique_ptr<UpdateScheduler> update_scheduler_;
  fxl::RefPtr<SessionForTest> session_;
  std::vector<fuchsia::ui::scenic::Event> events_;
};

}  // namespace test
}  // namespace gfx
}  // namespace scenic_impl

#endif  // GARNET_LIB_UI_GFX_TESTS_SESSION_TEST_H_
