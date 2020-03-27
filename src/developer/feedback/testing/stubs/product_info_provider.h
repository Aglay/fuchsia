// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_FEEDBACK_TESTING_STUBS_PRODUCT_INFO_PROVIDER_H_
#define SRC_DEVELOPER_FEEDBACK_TESTING_STUBS_PRODUCT_INFO_PROVIDER_H_

#include <fuchsia/hwinfo/cpp/fidl.h>
#include <fuchsia/hwinfo/cpp/fidl_test_base.h>
#include <lib/fidl/cpp/binding.h>
#include <lib/fidl/cpp/interface_handle.h>

#include "src/lib/fxl/logging.h"

namespace feedback {
namespace stubs {

// Stub fuchsia.hwinfo.Product service to return controlled response to GetInfo().
class ProductInfoProvider : public fuchsia::hwinfo::testing::Product_TestBase {
 public:
  ProductInfoProvider(fuchsia::hwinfo::ProductInfo&& info) : info_(std::move(info)) {}

  // Returns a request handler for a binding to this stub service.
  fidl::InterfaceRequestHandler<fuchsia::hwinfo::Product> GetHandler() {
    return [this](fidl::InterfaceRequest<fuchsia::hwinfo::Product> request) {
      binding_ =
          std::make_unique<fidl::Binding<fuchsia::hwinfo::Product>>(this, std::move(request));
    };
  }

  void CloseConnection();

  // |fuchsia::hwinfo::Product|
  void GetInfo(GetInfoCallback callback) override;

  // |fuchsia::hwinfo::testing::Product_TestBase|
  void NotImplemented_(const std::string& name) override {
    FXL_NOTIMPLEMENTED() << name << " is not implemented";
  }

 private:
  std::unique_ptr<fidl::Binding<fuchsia::hwinfo::Product>> binding_;
  fuchsia::hwinfo::ProductInfo info_;
  bool has_been_called_ = false;
};

class ProductInfoProviderNeverReturns : public ProductInfoProvider {
 public:
  ProductInfoProviderNeverReturns() : ProductInfoProvider(fuchsia::hwinfo::ProductInfo()) {}

  // |fuchsia::hwinfo::Product|
  void GetInfo(GetInfoCallback callback) override;
};

}  // namespace stubs
}  // namespace feedback

#endif  // SRC_DEVELOPER_FEEDBACK_TESTING_STUBS_PRODUCT_INFO_PROVIDER_H_
