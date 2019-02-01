// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/zxdb/client/setting_schema_definition.h"

#include "garnet/bin/zxdb/client/setting_schema.h"
#include "garnet/bin/zxdb/client/job_context.h"
#include "garnet/bin/zxdb/client/system.h"
#include "garnet/bin/zxdb/client/target.h"
#include "garnet/bin/zxdb/client/thread.h"

namespace zxdb {

void InitializeSchemas() {
  // Will initialize the schemas only once.
  static bool initialized = false;
  if (initialized)
    return;
  initialized = true;

  // Simply getting the schemas will create them, so we need to make sure we
  // get all of them.
  System::GetSchema();
  JobContext::GetSchema();
  Target::GetSchema();
  Thread::GetSchema();
}

}  // namespace zxdb
