// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_TESTS_COMMON_DEFS_H_
#define PERIDOT_TESTS_COMMON_DEFS_H_

namespace {

constexpr char kCommonNullModule[] = "common_null_module";
constexpr char kCommonNullAction[] = "com.google.fuchsia.common.null";

constexpr char kCommonNullModuleStarted[] = "common_null_module_started";
constexpr char kCommonNullModuleStopped[] = "common_null_module_stopped";

constexpr char kCommonActiveModule[] = "common_active_module";
constexpr char kCommonActiveAction[] = "com.google.fuchsia.common.active";

constexpr char kCommonActiveModuleStarted[] = "common_active_module_started";
constexpr char kCommonActiveModuleOngoing[] = "common_active_module_ongoing";
constexpr char kCommonActiveModuleStopped[] = "common_active_module_stopped";

}  // namespace

#endif  // PERIDOT_TESTS_COMMON_DEFS_H_
