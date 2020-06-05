// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_COBALT_BIN_SYSTEM_METRICS_TESTING_FAKE_LOG_STATS_FETCHER_H_
#define SRC_COBALT_BIN_SYSTEM_METRICS_TESTING_FAKE_LOG_STATS_FETCHER_H_

#include <lib/async/dispatcher.h>

#include "src/cobalt/bin/system-metrics/log_stats_fetcher.h"

namespace cobalt {

class FakeLogStatsFetcher : public LogStatsFetcher {
 public:
  FakeLogStatsFetcher(async_dispatcher_t* dispatcher);

  void AddErrorCount(int error_count);

  // Overridden from LogStatsFetcher:
  void FetchMetrics(MetricsCallback metrics_callback) override;

 private:
  async_dispatcher_t* dispatcher_;
  MetricsCallback metrics_callback_;
  int error_count_ = 0;
};

}  // namespace cobalt

#endif  // SRC_COBALT_BIN_SYSTEM_METRICS_TESTING_FAKE_LOG_STATS_FETCHER_H_
