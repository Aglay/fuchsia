// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>
#include <stdint.h>
#include <unistd.h>

#include <lib/zx/handle.h>
#include <lib/zx/job.h>
#include <lib/zx/process.h>
#include <lib/zx/thread.h>
#include <lib/zx/vmar.h>

#include <zircon/process.h>
#include <zircon/syscalls/debug.h>
#include <zircon/syscalls/exception.h>
#include <zircon/syscalls/policy.h>

#include <zxtest/zxtest.h>

namespace {

// Basic job operation is tested by core-tests.
static zx::job MakeJob() {
  zx::job job;
  if (zx::job::create(*zx::job::default_job(), 0u, &job) != ZX_OK)
    return zx::job();
  return job;
}

void InvalidCalls(uint32_t options) {
    // Null policy pointer.
    auto job = MakeJob();
    EXPECT_EQ(job.set_policy(options, ZX_JOB_POL_BASIC_V2, nullptr, 0u), ZX_ERR_INVALID_ARGS);
    EXPECT_EQ(job.set_policy(options, ZX_JOB_POL_BASIC_V2, nullptr, 1u), ZX_ERR_INVALID_ARGS);
    EXPECT_EQ(job.set_policy(options, ZX_JOB_POL_BASIC_V2, nullptr, 5u), ZX_ERR_INVALID_ARGS);

    zx_policy_basic_v2_t policy[] = {{ZX_POL_BAD_HANDLE, ZX_POL_ACTION_KILL, 0u}};
    EXPECT_EQ(job.set_policy(options, ZX_JOB_POL_BASIC_V2, policy, 1u), ZX_ERR_NOT_SUPPORTED);
    EXPECT_EQ(job.set_policy(options, ZX_JOB_POL_BASIC_V2, policy, 0u), ZX_ERR_INVALID_ARGS);
}

TEST(JobPolicyV2Test, InvalidCallsAbs) { InvalidCalls(ZX_JOB_POL_ABSOLUTE); }

TEST(JobPolicyV2Test, InvalidCallsRel) { InvalidCalls(ZX_JOB_POL_RELATIVE); }


}  // anonymous namespace
