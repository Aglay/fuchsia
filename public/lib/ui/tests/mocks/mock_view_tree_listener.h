// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APPS_MOZART_LIB_TESTS_MOCKS_MOCK_VIEW_TREE_LISTENER_H_
#define APPS_MOZART_LIB_TESTS_MOCKS_MOCK_VIEW_TREE_LISTENER_H_

#include "lib/ui/views/fidl/view_manager.fidl.h"
#include "lib/ui/views/fidl/views.fidl.h"
#include "lib/ftl/macros.h"

#include <functional>

namespace mozart {
namespace test {

class MockViewTreeListener : public mozart::ViewTreeListener {
 public:
  MockViewTreeListener();
  ~MockViewTreeListener();

 private:
  FTL_DISALLOW_COPY_AND_ASSIGN(MockViewTreeListener);
};

}  // namespace test
}  // namespace mozart

#endif  // APPS_MOZART_LIB_TESTS_MOCKS_MOCK_VIEW_TREE_LISTENER_H_
