// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APPS_MEDIA_SERVICES_FRAMEWORK_PARTS_NULL_SINK_H_
#define APPS_MEDIA_SERVICES_FRAMEWORK_PARTS_NULL_SINK_H_

#include "apps/media/services/framework/models/active_sink.h"

namespace mojo {
namespace media {

// Sink that throws packets away.
class NullSink : public ActiveSink {
 public:
  static std::shared_ptr<NullSink> Create() {
    return std::shared_ptr<NullSink>(new NullSink());
  }

  ~NullSink() override;

  // ActiveSink implementation.
  PayloadAllocator* allocator() override;

  void SetDemandCallback(const DemandCallback& demand_callback) override;

  Demand SupplyPacket(PacketPtr packet) override;

 private:
  NullSink();

  DemandCallback demand_callback_;
};

}  // namespace media
}  // namespace mojo

#endif  // APPS_MEDIA_SERVICES_FRAMEWORK_PARTS_NULL_SINK_H_
