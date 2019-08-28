/*
 * Copyright (c) 2019 The Fuchsia Authors
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdio.h>

#include <ddk/device.h>
#include <gtest/gtest.h>

#include "src/connectivity/wlan/drivers/testing/lib/sim-device/device.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/common.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/core.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/sim_device.h"

namespace wlan {
namespace brcmfmac {
namespace {

TEST(LifecycleTest, StartStop) {
  auto env = std::make_shared<simulation::Environment>();
  auto dev_mgr = std::make_shared<simulation::FakeDevMgr>();

  std::unique_ptr<SimDevice> device;
  zx_status_t status = SimDevice::Create(nullptr, dev_mgr, env, &device);
  ASSERT_EQ(status, ZX_OK);
}

}  // namespace
}  // namespace brcmfmac
}  // namespace wlan
