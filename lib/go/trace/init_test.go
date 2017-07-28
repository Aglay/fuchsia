// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package trace_test

import (
	"os"
	"path/filepath"
	"testing"

	"fidl/bindings"

	"apps/tracing/lib/trace"
	"apps/tracing/services/trace_provider"
	"apps/tracing/services/trace_registry"
)

type TraceRegistryMock struct {
	provider *trace_provider.Pointer
	label    string
	stub     *bindings.Stub
}

func (tr *TraceRegistryMock) RegisterTraceProvider(inProvider trace_provider.Pointer, inLabel *string) error {
	tr.provider = &inProvider
	tr.label = *inLabel
	return nil
}

type TraceRegistryMockDelegate struct {
	stub []*bindings.Stub
	tr   *TraceRegistryMock
}

func (trd *TraceRegistryMockDelegate) Bind(r trace_registry.Request) {
	if trd.tr == nil {
		trd.tr = &TraceRegistryMock{}
	}
	stub := trace_registry.NewStub(r, trd.tr, bindings.GetAsyncWaiter())
	trd.stub = append(trd.stub, stub)
}

func TestInitializeTracer(t *testing.T) {
	delegate := &TraceRegistryMockDelegate{}
	traceRegistryRequest, traceRegistryPointer := trace_registry.NewChannel()
	delegate.Bind(traceRegistryRequest)
	traceRegistryProxy := trace_registry.NewProxy(traceRegistryPointer, bindings.GetAsyncWaiter())

	trace.InitializeTracerUsingRegistry(traceRegistryProxy, trace.Setting{})

	if err := delegate.stub[0].ServeRequest(); err != nil {
		t.Fatal(err)
	}

	if delegate.tr.provider == nil {
		t.Fatal("Provider should not be null")
	}
	if delegate.tr.label != filepath.Base(os.Args[0]) {
		t.Fatalf("label should be %q, got %q", filepath.Base(os.Args[0]), delegate.tr.label)
	}
}
