// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/ledger/cobalt/cobalt.h"

#include <lib/fit/function.h>
#include <lib/fsl/vmo/file.h>

#include "peridot/lib/cobalt/cobalt.h"

namespace ledger {
namespace {
constexpr char kConfigBinProtoPath[] =
    "/pkg/data/ledger_cobalt_config.binproto";
constexpr int32_t kCobaltMetricId = 2;
constexpr int32_t kCobaltEncodingId = 2;

cobalt::CobaltContext* g_cobalt_context = nullptr;

}  // namespace

fxl::AutoCall<fit::closure> InitializeCobalt(
    async_dispatcher_t* dispatcher, component::StartupContext* context) {
  std::unique_ptr<cobalt::CobaltContext> cobalt_context;
  FXL_DCHECK(!g_cobalt_context);

  fsl::SizedVmo config;
  FXL_CHECK(fsl::VmoFromFilename(kConfigBinProtoPath, &config))
      << "Could not read Cobalt config file into VMO";

  cobalt_context =
      cobalt::MakeCobaltContext(dispatcher, context, std::move(config));
  g_cobalt_context = cobalt_context.get();
  return fxl::MakeAutoCall<fit::closure>(
      [cobalt_context = std::move(cobalt_context)] {
        g_cobalt_context = nullptr;
      });
}

void ReportEvent(CobaltEvent event) {
  // Do not do anything if cobalt reporting is disabled.
  if (!g_cobalt_context) {
    return;
  }
  fuchsia::cobalt::Value value;
  value.set_index_value(static_cast<uint32_t>(event));
  cobalt::CobaltObservation observation(kCobaltMetricId, kCobaltEncodingId,
                                        std::move(value));
  g_cobalt_context->ReportObservation(observation);
}

}  // namespace ledger
