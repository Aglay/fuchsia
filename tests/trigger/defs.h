// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_TESTS_TRIGGER_DEFS_H_
#define PERIDOT_TESTS_TRIGGER_DEFS_H_

namespace {

// This is how long we wait for the test to finish before we timeout and tear
// down our test.
constexpr int kTimeoutMilliseconds = 10000;
constexpr char kTestAgent[] =
    "file:///system/test/modular_tests/trigger_test_agent";

}  // namespace

#endif  // PERIDOT_TESTS_TRIGGER_DEFS_H_
