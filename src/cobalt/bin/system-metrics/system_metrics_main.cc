// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The cobalt system metrics collection daemon uses cobalt to log system metrics
// on a regular basis.

#include <memory>

#include <lib/async-loop/cpp/loop.h>
#include <lib/sys/cpp/component_context.h>

#include "src/cobalt/bin/system-metrics/system_metrics_daemon.h"

int main(int argc, const char** argv) {
  async::Loop loop(&kAsyncLoopConfigAttachToThread);
  auto context = sys::ComponentContext::Create();

  // Create the SystemMetricsDaemon and start it.
  SystemMetricsDaemon daemon(loop.dispatcher(), context.get());
  daemon.Work();
  loop.Run();
  return 0;
}
