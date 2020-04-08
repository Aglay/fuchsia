// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_FEEDBACK_TESTING_STUBS_NETWORK_REACHABILITY_PROVIDER_H_
#define SRC_DEVELOPER_FEEDBACK_TESTING_STUBS_NETWORK_REACHABILITY_PROVIDER_H_

#include <fuchsia/net/cpp/fidl.h>
#include <fuchsia/net/cpp/fidl_test_base.h>

#include "src/developer/feedback/testing/stubs/fidl_server.h"

namespace feedback {
namespace stubs {

class NetworkReachabilityProvider : public STUB_FIDL_SERVER(fuchsia::net, Connectivity) {
 public:
  // |fuchsia::net::Connectivity|
  void TriggerOnNetworkReachable(bool reachable);
};

}  // namespace stubs
}  // namespace feedback

#endif  // SRC_DEVELOPER_FEEDBACK_TESTING_STUBS_NETWORK_REACHABILITY_PROVIDER_H_
