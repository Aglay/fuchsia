// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/lib/ui/scenic/tests/mocks.h"

namespace scene_manager {
namespace test {

SessionForTest::SessionForTest(SessionId id,
                               Engine* engine,
                               scene_manager::EventReporter* event_reporter,
                               mz::ErrorReporter* error_reporter)
    : Session(id, engine, event_reporter, error_reporter) {}

void SessionForTest::TearDown() {
  Session::TearDown();
}

SessionHandlerForTest::SessionHandlerForTest(
    Engine* engine,
    SessionId session_id,
    ::f1dl::InterfaceRequest<scenic::Session> request,
    ::f1dl::InterfaceHandle<scenic::SessionListener> listener)
    : SessionHandler(engine,
                     session_id,
                     std::move(request),
                     std::move(listener)),
      enqueue_count_(0),
      present_count_(0) {}

void SessionHandlerForTest::Enqueue(::f1dl::Array<scenic::OpPtr> ops) {
  SessionHandler::Enqueue(std::move(ops));
  ++enqueue_count_;
}

void SessionHandlerForTest::Present(uint64_t presentation_time,
                                    ::f1dl::Array<zx::event> acquire_fences,
                                    ::f1dl::Array<zx::event> release_fences,
                                    const PresentCallback& callback) {
  SessionHandler::Present(presentation_time, std::move(acquire_fences),
                          std::move(release_fences), callback);
  ++present_count_;
}

ReleaseFenceSignallerForTest::ReleaseFenceSignallerForTest(
    escher::impl::CommandBufferSequencer* command_buffer_sequencer)
    : ReleaseFenceSignaller(command_buffer_sequencer) {}

void ReleaseFenceSignallerForTest::AddCPUReleaseFence(zx::event fence) {
  num_calls_to_add_cpu_release_fence_++;
  // Signal immediately for testing purposes.
  fence.signal(0u, escher::kFenceSignalled);
}

EngineForTest::EngineForTest(DisplayManager* display_manager,
                             std::unique_ptr<escher::ReleaseFenceSignaller> r,
                             escher::Escher* escher)
    : Engine(display_manager, std::move(r), escher) {}

std::unique_ptr<SessionHandler> EngineForTest::CreateSessionHandler(
    SessionId session_id,
    ::f1dl::InterfaceRequest<scenic::Session> request,
    ::f1dl::InterfaceHandle<scenic::SessionListener> listener) {
  return std::make_unique<SessionHandlerForTest>(
      this, session_id, std::move(request), std::move(listener));
}

}  // namespace test
}  // namespace scene_manager
