// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_CLOUD_PROVIDER_VALIDATION_TYPES_H_
#define LIB_CLOUD_PROVIDER_VALIDATION_TYPES_H_

#include <fuchsia/ledger/cloud/cpp/fidl.h>

// More convenient aliases for FIDL types.

namespace cloud_provider {
using CloudProvider = fuchsia::ledger::cloud::CloudProvider;
using CloudProviderPtr = fuchsia::ledger::cloud::CloudProviderPtr;
using CloudProviderSync2Ptr = fuchsia::ledger::cloud::CloudProviderSync2Ptr;
using Commit = fuchsia::ledger::cloud::Commit;
using DeviceSet = fuchsia::ledger::cloud::DeviceSet;
using DeviceSetPtr = fuchsia::ledger::cloud::DeviceSetPtr;
using DeviceSetSync2Ptr = fuchsia::ledger::cloud::DeviceSetSync2Ptr;
using DeviceSetWatcher = fuchsia::ledger::cloud::DeviceSetWatcher;
using DeviceSetWatcherPtr = fuchsia::ledger::cloud::DeviceSetWatcherPtr;
using PageCloud = fuchsia::ledger::cloud::PageCloud;
using PageCloudPtr = fuchsia::ledger::cloud::PageCloudPtr;
using PageCloudSync2Ptr = fuchsia::ledger::cloud::PageCloudSync2Ptr;
using PageCloudWatcher = fuchsia::ledger::cloud::PageCloudWatcher;
using PageCloudWatcherPtr = fuchsia::ledger::cloud::PageCloudWatcherPtr;
using Status = fuchsia::ledger::cloud::Status;
using Token = fuchsia::ledger::cloud::Token;
}  // namespace cloud_provider

#endif  // LIB_CLOUD_PROVIDER_VALIDATION_TYPES_H_
