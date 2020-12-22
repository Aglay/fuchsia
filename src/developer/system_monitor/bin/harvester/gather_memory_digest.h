// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_SYSTEM_MONITOR_BIN_HARVESTER_GATHER_MEMORY_DIGEST_H_
#define SRC_DEVELOPER_SYSTEM_MONITOR_BIN_HARVESTER_GATHER_MEMORY_DIGEST_H_

#include "gather_category.h"
#include "src/developer/memory/metrics/digest.h"
#include "src/developer/memory/metrics/summary.h"

namespace harvester {

// A memory digest builds a set of 'buckets', to group memory into logical
// categories. I.e. it creates a digest of the memory usage.
class GatherMemoryDigest : public GatherCategory {
 public:
  GatherMemoryDigest(zx_handle_t info_resource,
                     harvester::DockyardProxy* dockyard_proxy)
      : GatherCategory(info_resource, dockyard_proxy) {}

  // GatherCategory.
  void Gather() override;

 private:
  memory::Digester digester_;
  memory::Namer namer_{memory::Summary::kNameMatches};
};

}  // namespace harvester

#endif  // SRC_DEVELOPER_SYSTEM_MONITOR_BIN_HARVESTER_GATHER_MEMORY_DIGEST_H_
