// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include "src/developer/debug/shared/logging/file_line_function.h"
#include "src/lib/fxl/macros.h"
#include "src/lib/fxl/time/stopwatch.h"

namespace debug_ipc {

// BlockTimer ------------------------------------------------------------------

// Simple RAII-esque timer that prints the duration of a block if running on
// debug mode.
//
// Normally you would use it from the TIME_BLOCK macro (defined below), that
// will easily add the current calling site, but you can add your own locations
// in order to proxy calls (see message_loop.cc for an example).
class BlockTimer {
 public:
  BlockTimer(FileLineFunction origin);
  ~BlockTimer();  // Will log on destruction.

  // This is what get called on destruction. You can call it before destruction
  // to trigger the timer before that. Will not trigger again.
  void EndTimer();

  // BlockTimers should only measure the block they're in. No weird stuff.
  FXL_DISALLOW_COPY_AND_ASSIGN(BlockTimer);
  FXL_DISALLOW_MOVE(BlockTimer);

 private:
  FileLineFunction origin_;  // Where this timer was called from.
  fxl::Stopwatch timer_;
  bool should_log_;
};

// We use this macro to ensure the concatenation of the values. Oh macros :)
#define TIME_BLOCK_TOKEN(x, y) x##y
#define TIME_BLOCK_TOKEN2(x, y) TIME_BLOCK_TOKEN(x, y)

#define TIME_BLOCK() \
  ::debug_ipc::BlockTimer TIME_BLOCK_TOKEN2(block_timer_, __LINE__)(FROM_HERE)

}  // namespace debug_ipc
