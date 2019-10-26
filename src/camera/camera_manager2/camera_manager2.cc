// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>

#include "src/camera/camera_manager2/camera_manager_app.h"
#include "src/lib/syslog/cpp/logger.h"

int main() {
  syslog::InitLogger({"camera_manager"});

  FX_LOGS(INFO) << "Camera Manager Starting";
  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);
  auto context = sys::ComponentContext::Create();
  camera::CameraManagerApp app(std::move(context));
  loop.Run();
  return 0;
}
