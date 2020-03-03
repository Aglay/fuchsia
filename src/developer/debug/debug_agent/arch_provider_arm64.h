// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_DEBUG_AGENT_ARCH_PROVIDER_ARM64_H_
#define SRC_DEVELOPER_DEBUG_DEBUG_AGENT_ARCH_PROVIDER_ARM64_H_

#include "src/developer/debug/debug_agent/arch_provider_fuchsia.h"

namespace debug_agent {
namespace arch {

class ArchProviderArm64 : public ArchProviderFuchsia {
 public:
  void FillExceptionRecord(const zx::thread&, debug_ipc::ExceptionRecord* out) const override;
};

}  // namespace arch
}  // namespace debug_agent

#endif  // SRC_DEVELOPER_DEBUG_DEBUG_AGENT_ARCH_PROVIDER_ARM64_H_
