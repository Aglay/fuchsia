// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_DRIVERS_GPU_MSD_VSL_GC_INCLUDE_MAGMA_VENDOR_QUERIES_H_
#define GARNET_DRIVERS_GPU_MSD_VSL_GC_INCLUDE_MAGMA_VENDOR_QUERIES_H_

#include "magma_common_defs.h"

enum MsdVslVendorQuery {
  kMsdVslVendorQueryChipIdentity = MAGMA_QUERY_VENDOR_PARAM_0,
  kMsdVslVendorQueryChipOption = MAGMA_QUERY_VENDOR_PARAM_0 + 1,
};

#endif  // GARNET_DRIVERS_GPU_MSD_VSL_GC_INCLUDE_MAGMA_VENDOR_QUERIES_H_
