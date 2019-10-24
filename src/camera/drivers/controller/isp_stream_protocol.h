// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CAMERA_DRIVERS_CONTROLLER_ISP_STREAM_PROTOCOL_H_
#define SRC_CAMERA_DRIVERS_CONTROLLER_ISP_STREAM_PROTOCOL_H_

#include <lib/async-loop/cpp/loop.h>
#include <lib/async/cpp/task.h>
#include <lib/fidl/cpp/binding.h>
#include <zircon/compiler.h>

#include <queue>
#include <unordered_set>

#include <ddktl/protocol/isp.h>
#include <fbl/mutex.h>

namespace camera {

// ISP Stream Protocol Implementation
class IspStreamProtocol {
 public:
  IspStreamProtocol() : protocol_{&protocol_ops_, nullptr} {}

  // Returns a pointer to this instance's protocol parameter, to be populated via the Stream banjo
  // interface.
  output_stream_protocol_t* protocol() { return &protocol_; }

  void Start() {
    ZX_ASSERT(started_ == false);
    ZX_ASSERT(ZX_OK == protocol_.ops->start(protocol_.ctx));
    started_ = true;
  }

  void Stop() {
    ZX_ASSERT(started_);
    ZX_ASSERT(ZX_OK == protocol_.ops->stop(protocol_.ctx));
    started_ = false;
  }

  void ReleaseFrame(uint32_t buffer_id) {
    ZX_ASSERT(ZX_OK == protocol_.ops->release_frame(protocol_.ctx, buffer_id));
  }

 private:
  bool started_ = false;
  output_stream_protocol_t protocol_;
  output_stream_protocol_ops_t protocol_ops_;
};

}  // namespace camera

#endif  // SRC_CAMERA_DRIVERS_CONTROLLER_ISP_STREAM_PROTOCOL_H_
