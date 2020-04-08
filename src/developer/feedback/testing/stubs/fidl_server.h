// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_FEEDBACK_TESTING_STUBS_FIDL_SERVER_H_
#define SRC_DEVELOPER_FEEDBACK_TESTING_STUBS_FIDL_SERVER_H_

#include <lib/fidl/cpp/binding.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/fidl/cpp/interface_handle.h>
#include <lib/fidl/cpp/interface_request.h>

#include <memory>

#include "src/lib/fxl/logging.h"

namespace feedback {
namespace stubs {

template <typename Interface, typename TestBase>
class FidlServer : public TestBase {
 public:
  ::fidl::InterfaceRequestHandler<Interface> GetHandler() {
    return [this](::fidl::InterfaceRequest<Interface> request) {
      binding_ = std::make_unique<::fidl::Binding<Interface>>(this, std::move(request));
    };
  }

  void CloseConnection() {
    if (binding_) {
      binding_->Close(ZX_ERR_PEER_CLOSED);
    }
  }

  bool IsBound() const { return binding_->is_bound(); }

  void NotImplemented_(const std::string& name) override {
    FXL_NOTIMPLEMENTED() << name << " is not implemented";
  }

 protected:
  std::unique_ptr<::fidl::Binding<Interface>>& binding() { return binding_; }

 private:
  std::unique_ptr<::fidl::Binding<Interface>> binding_;
};

}  // namespace stubs
}  // namespace feedback

#define STUB_FIDL_SERVER(_1, _2) feedback::stubs::FidlServer<_1::_2, _1::testing::_2##_TestBase>

#define STUB_METHOD_DOES_NOT_RETURN(METHOD, PARAM_TYPES...) \
  void METHOD(PARAM_TYPES) override {}

#define STUB_METHOD_CLOSES_CONNECTION(METHOD, PARAM_TYPES...) \
  void METHOD(PARAM_TYPES) override { CloseConnection(); }

#endif  // SRC_DEVELOPER_FEEDBACK_TESTING_STUBS_FIDL_SERVER_H_
