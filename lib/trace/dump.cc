// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/tracing/lib/trace/dump.h"

#include "lib/ftl/logging.h"

namespace tracing {

Dump::Dump(mx::socket socket) : socket_(std::move(socket)) {}

Dump::~Dump() {
  // TODO(jeffbrown): Implement a custom streambuf in libmtl so we don't have
  // to copy to a string just to interoperate with iostreams.  Should also
  // write in chunks if the output is too big for the socket's buffer.
  std::string content = out_.str();
  mx_status_t status = NO_ERROR;
  for (size_t offset = 0u; offset < content.size();) {
    size_t actual;
    mx_status_t status = socket_.write(0u, content.data() + offset,
                                       content.size() - offset, &actual);
    if (status != NO_ERROR)
      break;

    offset += actual;

    mx_signals_t pending;
    status = socket_.wait_one(MX_SOCKET_WRITABLE | MX_SOCKET_PEER_CLOSED,
                              MX_TIME_INFINITE, &pending);
    if (status != NO_ERROR || !(pending & MX_SOCKET_WRITABLE))
      break;
  }

  if (status != NO_ERROR) {
    FTL_LOG(WARNING) << "Failed to write entire dump to socket: status="
                     << status;
  }
}

}  // namespace tracing
