// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lib/app/cpp/application_context.h"
#include "garnet/bin/media/net_media_service/net_media_service_impl.h"
#include "lib/mtl/tasks/message_loop.h"

int main(int argc, const char** argv) {
  mtl::MessageLoop loop;

  media::NetMediaServiceImpl impl(app::ApplicationContext::CreateFromStartupInfo());

  loop.Run();
  return 0;
}
