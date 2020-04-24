// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>

#include "src/speech/tts/tts_service_impl.h"

int main(int argc, const char** argv) {
  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);

  tts::TtsServiceImpl impl(sys::ComponentContext::CreateAndServeOutgoingDirectory());

  if (impl.Init() != ZX_OK)
    return -1;

  loop.Run();
  return 0;
}
