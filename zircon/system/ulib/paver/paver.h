// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <fbl/string.h>
#include <fbl/unique_fd.h>
#include <fuchsia/paver/llcpp/fidl.h>
#include <lib/zx/channel.h>
#include <zircon/types.h>

#include "abr.h"
#include "device-partitioner.h"

namespace paver {

class Paver : public ::llcpp::fuchsia::paver::Paver::Interface {
 public:
  void WriteAsset(::llcpp::fuchsia::paver::Configuration configuration,
                  ::llcpp::fuchsia::paver::Asset asset, ::llcpp::fuchsia::mem::Buffer payload,
                  WriteAssetCompleter::Sync completer) override;

  void WriteVolumes(zx::channel payload_stream, WriteVolumesCompleter::Sync completer) override;

  void WriteBootloader(::llcpp::fuchsia::mem::Buffer payload,
                       WriteBootloaderCompleter::Sync completer) override;

  void WriteDataFile(fidl::StringView filename, ::llcpp::fuchsia::mem::Buffer payload,
                     WriteDataFileCompleter::Sync completer) override;

  void WipeVolumes(zx::channel gpt_block_device, WipeVolumesCompleter::Sync completer) override;

  void InitializePartitionTables(zx::channel gpt_block_device,
                                 InitializePartitionTablesCompleter::Sync completer) override;

  void WipePartitionTables(zx::channel block_device,
                           WipePartitionTablesCompleter::Sync completer) override;

  void QueryActiveConfiguration(QueryActiveConfigurationCompleter::Sync completer) override;

  void SetActiveConfiguration(::llcpp::fuchsia::paver::Configuration configuration,
                              SetActiveConfigurationCompleter::Sync completer) override;

  void MarkActiveConfigurationSuccessful(
      MarkActiveConfigurationSuccessfulCompleter::Sync completer) override;

  void set_devfs_root(fbl::unique_fd devfs_root) { devfs_root_ = std::move(devfs_root); }
  void set_svc_root(zx::channel svc_root) { svc_root_ = std::move(svc_root); }

 private:
  bool InitializePartitioner(zx::channel block_device,
                             std::unique_ptr<DevicePartitioner>* partitioner);
  bool InitializePartitioner(std::unique_ptr<DevicePartitioner>* partitioner) {
    return InitializePartitioner(zx::channel(), partitioner);
  }
  zx_status_t InitializeAbrClient();

  // Used for test injection.
  fbl::unique_fd devfs_root_;
  zx::channel svc_root_;
  // Lazily initialized to allow tests to inject a fake devfs root after creating.
  std::unique_ptr<DevicePartitioner> partitioner_;
  // Lazily initialized to allow tests to inject a fake devfs root after creating.
  std::unique_ptr<abr::Client> abr_client_;
};

}  // namespace paver
