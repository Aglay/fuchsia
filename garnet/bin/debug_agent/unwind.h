// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <stdint.h>
#include <lib/zx/thread.h>
#include <vector>

#include "garnet/lib/debug_ipc/records.h"

namespace debug_agent {

// We're testing different unwinders, this specifies which one you want to use.
// The unwinder type is a process-wide state.
enum class UnwinderType { kNgUnwind, kAndroid };
void SetUnwinderType(UnwinderType unwinder_type);

zx_status_t UnwindStack(const zx::process& process, uint64_t dl_debug_addr,
                        const zx::thread& thread, uint64_t ip, uint64_t sp,
                        uint64_t bp, size_t max_depth,
                        std::vector<debug_ipc::StackFrame>* stack);

}  // namespace debug_agent
