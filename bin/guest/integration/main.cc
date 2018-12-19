// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <lib/fxl/logging.h>
#include <lib/fxl/strings/string_printf.h>

#include "guest_test.h"

using ::testing::HasSubstr;

static constexpr char kVirtioRngUtil[] = "virtio_rng_test_util";

template <class T>
T GuestTest<T>::enclosed_guest_;

class SingleCpuZirconEnclosedGuest : public ZirconEnclosedGuest {
  zx_status_t LaunchInfo(fuchsia::guest::LaunchInfo* launch_info) override {
    launch_info->url = kZirconGuestUrl;
    launch_info->args.push_back("--virtio-gpu=false");
    launch_info->args.push_back("--cpus=1");
    launch_info->args.push_back("--cmdline-add=kernel.serial=none");
    return ZX_OK;
  }
};

class SingleCpuLinuxEnclosedGuest : public LinuxEnclosedGuest {
  zx_status_t LaunchInfo(fuchsia::guest::LaunchInfo* launch_info) override {
    launch_info->url = kLinuxGuestUrl;
    launch_info->args.push_back("--virtio-gpu=false");
    launch_info->args.push_back("--cpus=1");
    launch_info->args.push_back(
        "--cmdline=loglevel=0 console=hvc0 root=/dev/vda rw");
    return ZX_OK;
  }
};

using GuestTypes =
    ::testing::Types<ZirconEnclosedGuest, SingleCpuZirconEnclosedGuest,
                     LinuxEnclosedGuest, SingleCpuLinuxEnclosedGuest>;
TYPED_TEST_CASE(GuestTest, GuestTypes);

TYPED_TEST(GuestTest, LaunchGuest) {
  std::string result;
  EXPECT_EQ(this->Execute("echo \"test\"", &result), ZX_OK);
  EXPECT_EQ(result, "test\n");
}

TYPED_TEST(GuestTest, VirtioRng) {
  std::string result;
  EXPECT_EQ(this->RunUtil(kVirtioRngUtil, "", &result), ZX_OK);
  EXPECT_THAT(result, HasSubstr("PASS"));
}
