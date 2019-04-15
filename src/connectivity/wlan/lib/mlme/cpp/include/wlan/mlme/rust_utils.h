// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_WLAN_LIB_MLME_CPP_INCLUDE_WLAN_MLME_RUST_UTILS_H_
#define SRC_CONNECTIVITY_WLAN_LIB_MLME_CPP_INCLUDE_WLAN_MLME_RUST_UTILS_H_

#include <src/connectivity/wlan/lib/mlme/rust/c-binding/bindings.h>

#include <memory>

namespace wlan {

using SequenceManager = std::unique_ptr<mlme_sequence_manager_t,
                                        void (*)(mlme_sequence_manager_t*)>;

SequenceManager NewSequenceManager();

}  // namespace wlan

#endif  // SRC_CONNECTIVITY_WLAN_LIB_MLME_CPP_INCLUDE_WLAN_MLME_RUST_UTILS_H_
