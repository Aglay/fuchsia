// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APPS_TRACING_LIB_TRACE_READER_H_
#define APPS_TRACING_LIB_TRACE_READER_H_

#include <stdint.h>

#include <unordered_map>
#include <vector>

#include "apps/tracing/lib/trace/internal/fields.h"
#include "apps/tracing/lib/trace/types.h"
#include "lib/ftl/macros.h"
#include "lib/ftl/strings/string_view.h"

namespace tracing {
namespace reader {

// Provides support for reading sequences of 64-bit words from a buffer.
class Chunk {
 public:
  Chunk();
  explicit Chunk(const uint64_t* begin, size_t num_words);

  uint64_t remaining_words() const { return end_ - current_; }

  // Reads from the chunk, maintaining proper alignment.
  // Returns true on success, false if the chunk has insufficient remaining
  // words to satisfy the request.
  bool Read(uint64_t* out_value);
  bool ReadInt64(int64_t* out_value);
  bool ReadDouble(double* out_value);
  bool ReadString(size_t length, ftl::StringView* out_string);
  bool ReadChunk(size_t num_words, Chunk* out_chunk);

 private:
  const uint64_t* current_;
  const uint64_t* end_;
};

// Callback invoked when decoding errors are detected in the trace.
using ErrorHandler = std::function<void(std::string)>;

// Retains context needed to decode traces.
class TraceContext {
 public:
  explicit TraceContext(ErrorHandler error_handler);
  ~TraceContext();

  // Reports a decoding error.
  void ReportError(std::string error) const;

  // Decodes a string reference from a chunk.
  bool DecodeStringRef(Chunk& chunk,
                       EncodedStringRef string_ref,
                       std::string* out_string) const;

  // Decodes a thread reference from a chunk.
  bool DecodeThreadRef(Chunk& chunk,
                       EncodedThreadRef thread_ref,
                       ProcessThread* out_process_thread) const;

  // Registers a string in the current string table.
  void RegisterString(StringIndex index, std::string string);

  // Registers a thread in the current thread table.
  void RegisterThread(ThreadIndex index, const ProcessThread& process_thread);

 private:
  ErrorHandler error_handler_;
  std::unordered_map<StringIndex, std::string> string_table_;
  std::unordered_map<ThreadIndex, ProcessThread> thread_table_;

  FTL_DISALLOW_COPY_AND_ASSIGN(TraceContext);
};

// A typed argument value.
class ArgumentValue {
 public:
  static ArgumentValue MakeNull() { return ArgumentValue(); }

  static ArgumentValue MakeInt32(int32_t value) { return ArgumentValue(value); }

  static ArgumentValue MakeInt64(int64_t value) { return ArgumentValue(value); }

  static ArgumentValue MakeDouble(double value) { return ArgumentValue(value); }

  static ArgumentValue MakeString(std::string value) {
    return ArgumentValue(std::move(value));
  }

  static ArgumentValue MakePointer(uintptr_t value) {
    return ArgumentValue(PointerTag(), value);
  }

  static ArgumentValue MakeKoid(uint64_t value) {
    return ArgumentValue(KoidTag(), value);
  }

  ArgumentValue(const ArgumentValue& other) : type_(other.type_) {
    Copy(other);
  }

  ~ArgumentValue() { Destroy(); }

  ArgumentValue& operator=(const ArgumentValue& rhs) {
    return Destroy().Copy(rhs);
  }

  ArgumentType type() const { return type_; }

  int32_t GetInt32() const {
    FTL_DCHECK(type_ == ArgumentType::kInt32);
    return int32_;
  }

  int64_t GetInt64() const {
    FTL_DCHECK(type_ == ArgumentType::kInt64);
    return int64_;
  }

  double GetDouble() const {
    FTL_DCHECK(type_ == ArgumentType::kDouble);
    return double_;
  }

  const std::string& GetString() const {
    FTL_DCHECK(type_ == ArgumentType::kString);
    return string_;
  }

  uintptr_t GetPointer() const {
    FTL_DCHECK(type_ == ArgumentType::kPointer);
    return uint64_;
  }

  uint64_t GetKoid() const {
    FTL_DCHECK(type_ == ArgumentType::kKoid);
    return uint64_;
  }

 private:
  struct PointerTag {};
  struct KoidTag {};

  ArgumentValue() : type_(ArgumentType::kNull) {}

  explicit ArgumentValue(int32_t int32)
      : type_(ArgumentType::kInt32), int32_(int32) {}

  explicit ArgumentValue(int64_t int64)
      : type_(ArgumentType::kInt64), int64_(int64) {}

  explicit ArgumentValue(double d) : type_(ArgumentType::kDouble), double_(d) {}

  explicit ArgumentValue(std::string string) : type_(ArgumentType::kString) {
    new (&string_) std::string(std::move(string));
  }

  explicit ArgumentValue(PointerTag, uintptr_t pointer)
      : type_(ArgumentType::kPointer), uint64_(pointer) {}

  explicit ArgumentValue(KoidTag, uint64_t koid)
      : type_(ArgumentType::kKoid), uint64_(koid) {}

  ArgumentValue& Destroy();
  ArgumentValue& Copy(const ArgumentValue& other);

  ArgumentType type_;
  union {
    int32_t int32_;
    int64_t int64_;
    double double_;
    std::string string_;
    uint64_t uint64_;
  };
};

// Named argument and value.
struct Argument {
  explicit Argument(std::string name, ArgumentValue value)
      : name(std::move(name)), value(std::move(value)) {}

  std::string name;
  ArgumentValue value;
};

// Event type specific data.
class EventData {
 public:
  // Duration begin event data.
  struct DurationBegin {};

  // Duration end event data.
  struct DurationEnd {};

  // Async begin event data.
  struct AsyncBegin {
    uint64_t id;
  };

  // Async instant event data.
  struct AsyncInstant {
    uint64_t id;
  };

  // Async end event data.
  struct AsyncEnd {
    uint64_t id;
  };

  explicit EventData(const DurationBegin& duration_begin)
      : type_(EventType::kDurationBegin), duration_begin_(duration_begin) {}

  explicit EventData(const DurationEnd& duration_end)
      : type_(EventType::kDurationEnd), duration_end_(duration_end) {}

  explicit EventData(const AsyncBegin& async_begin)
      : type_(EventType::kAsyncStart), async_begin_(async_begin) {}

  explicit EventData(const AsyncInstant& async_instant)
      : type_(EventType::kAsyncInstant), async_instant_(async_instant) {}

  explicit EventData(const AsyncEnd& async_end)
      : type_(EventType::kAsyncEnd), async_end_(async_end) {}

  const AsyncBegin& GetAsyncBegin() const {
    FTL_DCHECK(type_ == EventType::kAsyncStart);
    return async_begin_;
  };

  const AsyncInstant& GetAsyncInstant() const {
    FTL_DCHECK(type_ == EventType::kAsyncInstant);
    return async_instant_;
  }

  const AsyncEnd& GetAsyncEnd() const {
    FTL_DCHECK(type_ == EventType::kAsyncEnd);
    return async_end_;
  }

  EventType type() const { return type_; }

 private:
  EventType type_;
  union {
    DurationBegin duration_begin_;
    DurationEnd duration_end_;
    AsyncBegin async_begin_;
    AsyncInstant async_instant_;
    AsyncEnd async_end_;
  };
};

// A decoded record.
class Record {
 public:
  // Initialization record data.
  struct Initialization {
    uint64_t ticks_per_second;
  };

  // String record data.
  struct String {
    StringIndex index;
    std::string string;
  };

  // Thread record data.
  struct Thread {
    ThreadIndex index;
    ProcessThread process_thread;
  };

  // Event record data.
  struct Event {
    EventType type() const { return event_data.type(); }
    uint64_t timestamp;
    ProcessThread process_thread;
    std::string category;
    std::string name;
    std::vector<Argument> arguments;
    EventData event_data;
  };

  explicit Record(const Initialization& record)
      : type_(RecordType::kInitialization), initialization_(record) {}

  explicit Record(const String& record) : type_(RecordType::kString) {
    new (&string_) String(record);
  }

  explicit Record(const Thread& record) : type_(RecordType::kThread) {
    new (&thread_) Thread(record);
  }

  explicit Record(const Event& record) : type_(RecordType::kEvent) {
    new (&event_) Event(record);
  }

  Record(const Record& other) { Copy(other); }

  ~Record() { Destroy(); }

  Record& operator=(const Record& rhs) { return Destroy().Copy(rhs); }

  const Initialization& GetInitialization() const {
    FTL_DCHECK(type_ == RecordType::kInitialization);
    return initialization_;
  }

  const String& GetString() const {
    FTL_DCHECK(type_ == RecordType::kString);
    return string_;
  }

  const Thread& GetThread() const {
    FTL_DCHECK(type_ == RecordType::kThread);
    return thread_;
  };

  const Event& GetEvent() const {
    FTL_DCHECK(type_ == RecordType::kEvent);
    return event_;
  }

  RecordType type() const { return type_; }

 private:
  Record& Destroy();
  Record& Copy(const Record& other);

  RecordType type_;
  union {
    Initialization initialization_;
    String string_;
    Thread thread_;
    Event event_;
  };
};

// Called once for each record read by |ReadRecords|.
// TODO(jeffbrown): It would be nice to get rid of this by making |ReadRecords|
// return std::optional<Record> as an out parameter.
using RecordConsumer = std::function<void(const Record&)>;

// Reads trace records.
class TraceReader {
 public:
  explicit TraceReader(RecordConsumer record_consumer,
                       ErrorHandler error_handler);

  // Reads as many records as possible from the chunk, invoking the
  // record consumer for each one.  Returns true if the stream could possibly
  // contain more records if the chunk were extended with new data.
  // Returns false if the trace stream is unrecoverably corrupt and no
  // further decoding is possible.  May be called repeatedly with new
  // chunks as they become available to resume decoding.
  bool ReadRecords(Chunk& chunk);

 private:
  bool ReadInitializationRecord(Chunk& record,
                                ::tracing::internal::RecordHeader header);
  bool ReadStringRecord(Chunk& record,
                        ::tracing::internal::RecordHeader header);
  bool ReadThreadRecord(Chunk& record,
                        ::tracing::internal::RecordHeader header);
  bool ReadEventRecord(Chunk& record, ::tracing::internal::RecordHeader header);
  bool ReadArguments(Chunk& record,
                     size_t count,
                     std::vector<Argument>* out_arguments);

  RecordConsumer record_consumer_;
  TraceContext context_;
  ::tracing::internal::RecordHeader pending_header_ = 0u;
};

}  // namespace reader
}  // namespace tracing

#endif  // APPS_TRACING_LIB_TRACE_READER_H_
