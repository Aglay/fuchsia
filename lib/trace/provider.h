// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APPS_TRACING_LIB_TRACE_PROVIDER_H_
#define APPS_TRACING_LIB_TRACE_PROVIDER_H_

#include "application/lib/app/application_context.h"
#include "apps/tracing/lib/trace/dump.h"
#include "apps/tracing/lib/trace/settings.h"
#include "apps/tracing/services/trace_registry.fidl.h"

namespace tracing {

// Parses and applies settings from standard command-line options.
// Returns false and leaves the trace subsystem uninitialized if
// there was an error parsing the options.
bool InitializeTracerFromCommandLine(
    app::ApplicationContext* app_context,
    const ftl::CommandLine& command_line,
    const TraceSettings default_settings = TraceSettings());

// Initializes the global |Tracer| and registers its underlying
// |TraceProvider| with the system default trace provider registry service.
// Does not take ownership of |app_connector|.
void InitializeTracer(app::ApplicationContext* app_context,
                      const TraceSettings& settings);

// Initializes the global |Tracer| and registers its underlying
// |TraceProvider| with the specified |registry|.
void InitializeTracer(TraceRegistryPtr registry, const TraceSettings& settings);

// Destroys the global Tracer if one has been initialized with
// |InitializeTracer|.
void DestroyTracer();

// Sets the dump state callback.
// This must only be called after the tracer has been initialized.
void SetDumpCallback(DumpCallback callback);

}  // namespace tracing

#endif  // APPS_TRACING_LIB_TRACE_PROVIDER_H_
