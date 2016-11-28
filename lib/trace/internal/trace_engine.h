// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APPS_TRACING_LIB_TRACE_INTERNAL_TRACE_ENGINE_H_
#define APPS_TRACING_LIB_TRACE_INTERNAL_TRACE_ENGINE_H_

#include "stdint.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include <unordered_map>

#include "apps/tracing/lib/trace/writer.h"
#include "lib/ftl/strings/string_view.h"
#include "lib/ftl/macros.h"
#include "lib/ftl/memory/weak_ptr.h"
#include "lib/mtl/vmo/shared_vmo.h"
#include "lib/mtl/tasks/message_loop.h"
#include "lib/mtl/tasks/message_loop_handler.h"

namespace tracing {
namespace internal {

// Manages a single tracing session.
// The trace engine uses thread local state to maintain string and thread
// tables but it can tolerate having multiple instances alive at the same
// time though the performance of older instances will degrade.
//
// The trace engine is thread-safe but must be created on a |MessageLoop|
// thread which it uses to observe signals on the buffer's fence and to
// dispatch callbacks.
//
// (Unfortuately other parts of the trace system may not be so lucky.)
class TraceEngine final : private mtl::MessageLoopHandler {
 public:
  using Payload = ::tracing::writer::Payload;
  using StringRef = ::tracing::writer::StringRef;
  using ThreadRef = ::tracing::writer::ThreadRef;
  using TraceDisposition = ::tracing::writer::TraceDisposition;
  using TraceFinishedCallback = ::tracing::writer::TraceFinishedCallback;

  ~TraceEngine();

  // Creates and initializes the trace engine.
  // Must be called on a |MessageLoop| thread.
  // Returns nullptr if the engine could not be created.
  static std::unique_ptr<TraceEngine> Create(
      mx::vmo buffer,
      mx::eventpair fence,
      std::vector<std::string> enabled_categories);

  const ftl::RefPtr<ftl::TaskRunner>& task_runner() const {
    return task_runner_;
  }

  const std::vector<std::string>& enabled_categories() const {
    return enabled_categories_;
  }

  void StartTracing(TraceFinishedCallback finished_callback);
  void StopTracing();

  bool IsCategoryEnabled(const char* category) const;

  StringRef RegisterString(const char* constant, bool check_category);
  StringRef RegisterStringCopy(const std::string& string);
  ThreadRef RegisterCurrentThread();
  ThreadRef RegisterThread(mx_koid_t process_koid, mx_koid_t thread_koid);

  void WriteProcessDescription(mx_koid_t process_koid,
                               const std::string& process_name);
  void WriteThreadDescription(mx_koid_t process_koid,
                              mx_koid_t thread_koid,
                              const std::string& thread_name);

  void WriteInitializationRecord(uint64_t ticks_per_second);
  void WriteStringRecord(StringIndex index, const char* value);
  void WriteThreadRecord(ThreadIndex index,
                         mx_koid_t process_koid,
                         mx_koid_t thread_koid);
  Payload WriteEventRecordBase(EventType type,
                               const ThreadRef& thread_ref,
                               const StringRef& category_ref,
                               const StringRef& name_ref,
                               size_t argument_count,
                               size_t payload_size);
  Payload WriteKernelObjectRecordBase(mx_handle_t handle,
                                      size_t argument_count,
                                      size_t payload_size);
  Payload WriteKernelObjectRecordBase(mx_koid_t koid,
                                      mx_obj_type_t object_type,
                                      const StringRef& name_ref,
                                      size_t argument_count,
                                      size_t payload_size);

 private:
  explicit TraceEngine(ftl::RefPtr<mtl::SharedVmo> buffer,
                       mx::eventpair fence,
                       std::vector<std::string> enabled_categories);

  // |mtl::MessageLoopHandler|
  void OnHandleReady(mx_handle_t handle, mx_signals_t pending) override;
  void OnHandleError(mx_handle_t handle, mx_status_t error) override;

  ThreadRef RegisterThreadInternal(mx_koid_t process_koid,
                                   mx_koid_t thread_koid);
  Payload AllocateRecord(size_t num_bytes);

  void StopTracing(TraceDisposition disposition, bool immediate);
  void StopTracingOnMessageLoop(TraceDisposition disposition);

  uint32_t const generation_;

  ftl::RefPtr<mtl::SharedVmo> const buffer_;
  uintptr_t const buffer_start_;
  uintptr_t const buffer_end_;
  std::atomic<uintptr_t> buffer_current_;
  mx::eventpair const fence_;

  // We must keep both the vector and the set since the set contains
  // string views into the strings which are backed by the vector.
  std::vector<std::string> const enabled_categories_;
  std::set<ftl::StringView> enabled_category_set_;

  ftl::RefPtr<ftl::TaskRunner> const task_runner_;
  mtl::MessageLoop::HandlerKey fence_handler_key_{};

  TraceFinishedCallback finished_callback_;

  std::atomic<StringIndex> next_string_index_{1u};
  std::atomic<ThreadIndex> next_thread_index_{1u};

  enum class State { kCollecting, kAwaitingFinish };
  std::atomic<State> state_{State::kCollecting};

  std::mutex table_mutex_;
  std::unordered_map<std::string, StringRef>
      copied_string_table_;  // guarded by table_mutex_
  std::vector<std::unique_ptr<std::string>>
      copied_string_content_;  // guarded by table_mutex_
  std::unordered_map<ProcessThread, ThreadRef>
      process_thread_table_;  // guarded by table_mutex_

  ftl::WeakPtrFactory<TraceEngine> weak_ptr_factory_;

  FTL_DISALLOW_COPY_AND_ASSIGN(TraceEngine);
};

}  // namespace internal
}  // namespace tracing

#endif  // APPS_TRACING_LIB_TRACE_INTERNAL_TRACE_ENGINE_H_
