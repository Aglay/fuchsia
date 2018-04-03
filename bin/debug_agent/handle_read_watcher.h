// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

namespace debug_agent {

// Abstract class providing a notification when a handle transitions to
// readable.
class HandleReadWatcher {
 public:
  virtual void OnHandleReadable() = 0;
};

}  // namespace debug_agent
