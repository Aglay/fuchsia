// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CAMERA_EXAMPLES_DEMO_STREAM_PROVIDER_ISP_STREAM_PROVIDER_H_
#define SRC_CAMERA_EXAMPLES_DEMO_STREAM_PROVIDER_ISP_STREAM_PROVIDER_H_

#include <fbl/unique_fd.h>

#include "stream_provider.h"

class IspStreamProvider : public StreamProvider {
 public:
  static std::unique_ptr<StreamProvider> Create();
  virtual zx_status_t ConnectToStream(fidl::InterfaceRequest<fuchsia::camera2::Stream> request,
                                      fuchsia::sysmem::ImageFormat_2* format_out,
                                      fuchsia::sysmem::BufferCollectionInfo_2* buffers_out,
                                      bool* should_rotate_out) override;
  virtual std::string GetName() override { return "Image Signal Processor (ISP)"; }

 private:
  fbl::unique_fd isp_fd_;
};

#endif  // SRC_CAMERA_EXAMPLES_DEMO_STREAM_PROVIDER_ISP_STREAM_PROVIDER_H_
