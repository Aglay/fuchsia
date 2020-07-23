// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_DEBUG_AGENT_MOCK_SYSTEM_INTERFACE_H_
#define SRC_DEVELOPER_DEBUG_DEBUG_AGENT_MOCK_SYSTEM_INTERFACE_H_

#include <memory>

#include "src/developer/debug/debug_agent/mock_job_handle.h"
#include "src/developer/debug/debug_agent/system_interface.h"

namespace debug_agent {

class MockSystemInterface final : public SystemInterface {
 public:
  explicit MockSystemInterface(MockJobHandle root_job) : root_job_(std::move(root_job)) {}

  // SystemInterface implementation:
  std::unique_ptr<JobHandle> GetRootJob() const override;
  std::unique_ptr<JobHandle> GetComponentRootJob() const override;
  std::unique_ptr<BinaryLauncher> GetLauncher() const override;

 private:
  MockJobHandle root_job_;
};

}  // namespace debug_agent

#endif  // SRC_DEVELOPER_DEBUG_DEBUG_AGENT_MOCK_SYSTEM_INTERFACE_H_
