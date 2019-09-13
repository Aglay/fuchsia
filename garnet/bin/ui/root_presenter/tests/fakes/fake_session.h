// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_BIN_UI_ROOT_PRESENTER_TESTS_FAKES_FAKE_SESSION_H_
#define GARNET_BIN_UI_ROOT_PRESENTER_TESTS_FAKES_FAKE_SESSION_H_

#include <fuchsia/ui/scenic/cpp/fidl.h>
#include <fuchsia/ui/scenic/cpp/fidl_test_base.h>
#include <lib/fidl/cpp/binding.h>
#include <lib/ui/scenic/cpp/resources.h>

namespace root_presenter {
namespace testing {

class FakeSession : public fuchsia::ui::scenic::Session {
 public:
  FakeSession();

  ~FakeSession() override;

  //  void NotImplemented_(const std::string& name) final {}

  // Binds the session.
  void Bind(fidl::InterfaceRequest<fuchsia::ui::scenic::Session> request,
            fuchsia::ui::scenic::SessionListenerPtr listener);

  // Session implementation.
  void Enqueue(std::vector<fuchsia::ui::scenic::Command> cmds) override;

  void Present(uint64_t presentation_time, std::vector<zx::event> acquire_fences,
               std::vector<zx::event> release_fences, PresentCallback callback) override;
  void Present(uint64_t presentation_time, PresentCallback callback);
  void SetDebugName(std::string debug_name) override {}

 private:
  fidl::Binding<fuchsia::ui::scenic::Session> binding_;
  fuchsia::ui::scenic::SessionListenerPtr listener_;
};

}  // namespace testing
}  // namespace root_presenter

#endif  // GARNET_BIN_UI_ROOT_PRESENTER_TESTS_FAKES_FAKE_SESSION_H_
