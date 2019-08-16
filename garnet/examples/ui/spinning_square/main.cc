// Copyright 2015 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-loop/cpp/loop.h>
#include <lib/trace-provider/provider.h>
#include <lib/ui/base_view/cpp/view_provider_component_transitional.h>

#include "garnet/examples/ui/spinning_square/spinning_square_view.h"

int main(int argc, const char** argv) {
  async::Loop loop(&kAsyncLoopConfigAttachToThread);
  trace::TraceProviderWithFdio trace_provider(loop.dispatcher());

  scenic::ViewProviderComponentTransitional component(
      [](scenic::ViewContextTransitional context) {
        return std::make_unique<examples::SpinningSquareView>(std::move(context));
      },
      &loop);

  loop.Run();
  return 0;
}
