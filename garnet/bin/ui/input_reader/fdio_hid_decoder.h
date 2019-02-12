// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_BIN_UI_INPUT_READER_FDIO_HID_DECODER_H_
#define GARNET_BIN_UI_INPUT_READER_FDIO_HID_DECODER_H_

#include <string>
#include <vector>

#include <fbl/unique_fd.h>
#include <fuchsia/ui/input/cpp/fidl.h>

#include "garnet/bin/ui/input_reader/hid_decoder.h"
#include "garnet/bin/ui/input_reader/mouse.h"
#include "garnet/bin/ui/input_reader/touchscreen.h"

namespace fzl {
class FdioCaller;
}

namespace hid {
struct ReportField;
}

namespace mozart {

// This is the "real" FDIO implementation of |HidDecoder|.
class FdioHidDecoder : public HidDecoder {
 public:
  FdioHidDecoder(const std::string& name, fbl::unique_fd fd);
  ~FdioHidDecoder() override;

  // |HidDecoder|
  const std::string& name() const override { return name_; }

  // |HidDecoder|
  bool Init() override;
  // |HidDecoder|
  zx::event GetEvent() override;
  // |HidDecoder|
  BootMode ReadBootMode() const override { return boot_mode_; }
  // |HidDecoder|
  void SetupDevice(Device device) override;
  // |HidDecoder|
  const std::vector<uint8_t>& ReadReportDescriptor(int* bytes_read) override;
  // |HidDecoder|
  const std::vector<uint8_t>& Read(int* bytes_read) override;

 private:
  fbl::unique_fd fd_;
  const std::string name_;
  BootMode boot_mode_ = BootMode::NONE;
  std::vector<uint8_t> report_;
  std::vector<uint8_t> report_descriptor_;
};

}  // namespace mozart

#endif  // GARNET_BIN_UI_INPUT_READER_FDIO_HID_DECODER_H_
