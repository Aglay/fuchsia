// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/debug_agent/test_data/test_so_symbols.h"

#include <stdio.h>

void InsertBreakpointFunction() {
  printf("Breakpoint function!\n");
}
