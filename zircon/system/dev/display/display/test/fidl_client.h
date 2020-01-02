// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZIRCON_SYSTEM_DEV_DISPLAY_DISPLAY_TEST_FIDL_CLIENT_H_
#define ZIRCON_SYSTEM_DEV_DISPLAY_DISPLAY_TEST_FIDL_CLIENT_H_

#include <fuchsia/hardware/display/llcpp/fidl.h>
#include <fuchsia/sysmem/llcpp/fidl.h>
#include <lib/async/cpp/wait.h>
#include <lib/fidl/cpp/message.h>
#include <zircon/pixelformat.h>
#include <zircon/types.h>

#include <memory>

#include "base.h"

namespace display {

class TestFidlClient {
 public:
  class Display {
   public:
    Display(const ::llcpp::fuchsia::hardware::display::Info& info);

    uint64_t id_;
    fbl::Vector<zx_pixel_format_t> pixel_formats_;
    fbl::Vector<::llcpp::fuchsia::hardware::display::Mode> modes_;
    fbl::Vector<::llcpp::fuchsia::hardware::display::CursorInfo> cursors_;

    fbl::String manufacturer_name_;
    fbl::String monitor_name_;
    fbl::String monitor_serial_;

    ::llcpp::fuchsia::hardware::display::ImageConfig image_config_;
  };

  TestFidlClient(::llcpp::fuchsia::sysmem::Allocator::SyncClient* sysmem) : sysmem_(sysmem) {}
  ~TestFidlClient();

  bool CreateChannel(zx_handle_t provider, bool is_vc);
  // Enable vsync for a display and wait for events using |dispatcher|.
  bool Bind(async_dispatcher_t* dispatcher);
  zx_status_t ImportImageWithSysmem(
      const ::llcpp::fuchsia::hardware::display::ImageConfig& image_config, uint64_t* image_id);
  zx_status_t PresentImage();
  uint64_t display_id() const;

  fbl::Vector<Display> displays_;
  std::unique_ptr<::llcpp::fuchsia::hardware::display::Controller::SyncClient> dc_;
  ::llcpp::fuchsia::sysmem::Allocator::SyncClient* sysmem_;
  zx::handle device_handle_;
  bool has_ownership_ = false;
  size_t vsync_count_ = 0;
  uint64_t image_id_ = 0;
  uint64_t layer_id_ = 0;

 private:
  void OnEventMsgAsync(async_dispatcher_t* dispatcher, async::WaitBase* self, zx_status_t status,
                       const zx_packet_signal_t* signal);
  async::WaitMethod<TestFidlClient, &TestFidlClient::OnEventMsgAsync> wait_events_{this};
};

}  // namespace display

#endif  // ZIRCON_SYSTEM_DEV_DISPLAY_DISPLAY_TEST_FIDL_CLIENT_H_
