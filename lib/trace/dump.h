// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APPS_TRACING_LIB_TRACE_DUMP_H_
#define APPS_TRACING_LIB_TRACE_DUMP_H_

#include <mx/socket.h>

#include <functional>
#include <memory>
#include <sstream>

#include "lib/ftl/macros.h"

namespace tracing {

// Helper for dumping state in a human-readable form.
class Dump {
 public:
  explicit Dump(mx::socket socket);
  ~Dump();

  std::ostream& out() { return out_; }

 private:
  mx::socket socket_;
  std::ostringstream out_;

  FTL_DISALLOW_COPY_AND_ASSIGN(Dump);
};

// Callback to dump the state of the provider in a human-readable form.
using DumpCallback = std::function<void(std::unique_ptr<Dump> dump)>;

}  // namespace tracing

#endif  // APPS_TRACING_LIB_TRACE_DUMP_H_
