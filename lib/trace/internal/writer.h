// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APPS_TRACING_LIB_TRACE_INTERNAL_WRITER_H_
#define APPS_TRACING_LIB_TRACE_INTERNAL_WRITER_H_

#include <stdint.h>

#include <atomic>
#include <string>
#include <type_traits>
#include <utility>

#include <mx/vmo.h>

#include "apps/tracing/lib/trace/internal/types.h"

namespace tracing {
namespace internal {

template <typename T>
inline typename std::underlying_type<T>::type ToUnderlyingType(T value) {
  return static_cast<typename std::underlying_type<T>::type>(value);
}

struct Payload {
  static Payload New(size_t size);

  explicit Payload(uint64_t* ptr) : ptr(ptr) {}

  explicit operator bool() const { return ptr; }

  Payload& Write(uint64_t value) {
    *ptr++ = value;
    return *this;
  }

  Payload& WriteBytes(const void* src, size_t size) {
    memcpy(ptr, src, size);
    ptr += size >> 3;
    return *this;
  }

  template <typename T>
  Payload& WriteValue(const T& value) {
    value.WriteTo(*this);
    return *this;
  }

  template <typename Head, typename... Tail>
  Payload& WriteValues(Head&& head, Tail&&... tail) {
    WriteValue(head);
    return WriteValues(std::forward<Tail>(tail)...);
  }

  Payload& WriteValues() { return *this; }

  uint64_t* ptr;
};

struct Koid {
  explicit Koid(uint64_t value) : value(value) {}

  uint64_t value;
};

struct StringRef {
  static StringRef MakeEmpty() {
    return StringRef{StringRefFields::kEmpty, nullptr};
  }

  static StringRef MakeInlined(const char* string, size_t length) {
    if (!length)
      return MakeEmpty();
    uint16_t trim = std::min(length, size_t(StringRefFields::kMaxLength));
    return StringRef{static_cast<uint16_t>(trim | StringRefFields::kInlineFlag),
                     string};
  }

  // Constructs a |StringRef| from a std::string.
  // Lifetime of |string| must exceed the lifetime of the
  // returned |StringRef| instance.
  static StringRef MakeInlined(const std::string& string) {
    return MakeInlined(string.c_str(), string.size());
  }

  static StringRef MakeIndexed(uint16_t index) {
    return StringRef{index, nullptr};
  }

  size_t Size() const {
    return is_inlined() ? Pad(StringRefFields::kLengthMask & encoded) : 0;
  }

  void WriteTo(Payload& payload) const {
    if (is_inlined())
      payload.WriteBytes(string, Size());
  }

  bool is_empty() const { return encoded == StringRefFields::kEmpty; }
  bool is_indexed() const { return !is_empty() && !is_inlined(); }
  bool is_inlined() const { return encoded & StringRefFields::kInlineFlag; }

  uint16_t encoded;
  const char* string;
};

struct ThreadRef {
  size_t Size() const {
    return index == ThreadRefFields::kInline ? 2 * sizeof(uint64_t) : 0;
  }

  void WriteTo(Payload& payload) const {
    if (index == ThreadRefFields::kInline)
      payload.Write(process_koid).Write(thread_koid);
  }

  uint16_t index;
  uint64_t process_koid;
  uint64_t thread_koid;
};

StringRef RegisterString(const char* string);

ThreadRef RegisterCurrentThread();

void WriteInitializationRecord(uint64_t ticks_per_second);

void WriteStringRecord(uint16_t index, const char* string);

void WriteThreadRecord(uint16_t index,
                       uint64_t process_koid,
                       uint64_t thread_koid);

Payload WriteEventRecord(TraceEventType event_type,
                         const char* category,
                         const char* name,
                         size_t argument_count,
                         uint64_t payload_size);

inline size_t SizeArguments() {
  return 0;
}

template <typename Head, typename... Tail>
inline size_t SizeArguments(Head&& head, Tail&&... tail) {
  return head.Size() + SizeArguments(std::forward<Tail>(tail)...);
}

struct ArgumentBase {
  explicit ArgumentBase(const char* name) : name_ref(RegisterString(name)) {}

  size_t Size() const { return sizeof(uint64_t) + name_ref.Size(); }

  void WriteTo(Payload& payload,
               ArgumentType type,
               size_t size,
               ArgumentHeader extras = 0) const {
    payload
        .Write(ArgumentFields::Type::Make(ToUnderlyingType(type)) |
               ArgumentFields::ArgumentSize::Make(size >> 3) |
               ArgumentFields::NameRef::Make(name_ref.encoded) | extras)
        .WriteValue(name_ref);
  }

  StringRef name_ref;
};

template <typename T>
struct Argument;

template <>
struct Argument<int32_t> : ArgumentBase {
  explicit Argument(const char* name, int32_t value)
      : ArgumentBase(name), value(value) {}

  size_t Size() const { return ArgumentBase::Size(); }

  void WriteTo(Payload& payload) const {
    ArgumentBase::WriteTo(payload, ArgumentType::kInt32, Size(),
                          Int32ArgumentFields::Value::Make(value));
  }

  int32_t value;
};

template <>
struct Argument<int64_t> : ArgumentBase {
  explicit Argument(const char* name, int64_t value)
      : ArgumentBase(name), value(value) {}

  size_t Size() const { return ArgumentBase::Size() + sizeof(int64_t); }

  void WriteTo(Payload& payload) const {
    ArgumentBase::WriteTo(payload, ArgumentType::kInt64, Size());
    payload.Write(value);
  }

  int64_t value;
};

template <>
struct Argument<Koid> : ArgumentBase {
  explicit Argument(const char* name, const Koid& koid)
      : ArgumentBase(name), koid(koid) {}

  size_t Size() const { return ArgumentBase::Size() + sizeof(uint64_t); }

  void WriteTo(Payload& payload) const {
    ArgumentBase::WriteTo(payload, ArgumentType::kKernelObjectId, Size());
    payload.Write(koid.value);
  }

  Koid koid;
};

template <>
struct Argument<double> : ArgumentBase {
  explicit Argument(const char* name, double value)
      : ArgumentBase(name), value(value) {}

  size_t Size() const { return ArgumentBase::Size() + sizeof(double); }

  void WriteTo(Payload& payload) const {
    ArgumentBase::WriteTo(payload, ArgumentType::kDouble, Size());
    payload.WriteBytes(&value, sizeof(double));
  }

  double value;
};

template <>
struct Argument<std::string> : ArgumentBase {
  explicit Argument(const char* name, const std::string& value)
      : ArgumentBase(name),
        value_ref(StringRef::MakeInlined(value.c_str(), value.size())) {}

  size_t Size() const { return ArgumentBase::Size() + value_ref.Size(); }

  void WriteTo(Payload& payload) const {
    ArgumentBase::WriteTo(payload, ArgumentType::kString, Size(),
                          StringArgumentFields::Index::Make(value_ref.encoded));
    payload.WriteValue(value_ref);
  }

  StringRef value_ref;
};

template <>
struct Argument<const char*> : ArgumentBase {
  explicit Argument(const char* name, const char* value)
      : ArgumentBase(name), value_ref(RegisterString(value)) {}

  size_t Size() const { return ArgumentBase::Size() + value_ref.Size(); }

  void WriteTo(Payload& payload) const {
    ArgumentBase::WriteTo(payload, ArgumentType::kString, Size(),
                          StringArgumentFields::Index::Make(value_ref.encoded));
    payload.WriteValue(value_ref);
  }

  StringRef value_ref;
};

template <size_t n>
struct Argument<char[n]> : ArgumentBase {
  explicit Argument(const char* name, const char* value)
      : ArgumentBase(name), value_ref(RegisterString(value)) {}

  size_t Size() const { return ArgumentBase::Size() + value_ref.Size(); }

  void WriteTo(Payload& payload) const {
    ArgumentBase::WriteTo(payload, ArgumentType::kString, Size(),
                          StringArgumentFields::Index::Make(value_ref.encoded));
    payload.WriteValue(value_ref);
  }

  StringRef value_ref;
};

template <typename T>
struct Argument<T*> : ArgumentBase {
  explicit Argument(const char* name, const T* value)
      : ArgumentBase(name), value(value) {}

  size_t Size() const { return ArgumentBase::Size() + sizeof(uint64_t); }

  void WriteTo(Payload& payload) const {
    ArgumentBase::WriteTo(payload, ArgumentType::kPointer, Size());
    payload.Write(reinterpret_cast<uintptr_t>(value));
  }

  const T* value;
};

template <typename T, typename Enable = void>
struct ArgumentMaker {
  using ResultType = Argument<T>;
  static ResultType Make(const char* name, const T& value) {
    return ResultType(name, value);
  }
};

template <typename T>
struct ArgumentMaker<T, typename std::enable_if<std::is_enum<T>::value>::type> {
  using UnderlyingType = typename std::underlying_type<T>::type;
  using NumericType =
      typename std::conditional<sizeof(UnderlyingType) < sizeof(int32_t),
                                int32_t,
                                int64_t>::type;
  using ResultType = Argument<NumericType>;
  static ResultType Make(const char* name, T value) {
    return ResultType(name, static_cast<NumericType>(value));
  }
};

template <typename T>
struct ArgumentMaker<
    T,
    typename std::enable_if<std::is_unsigned<T>::value>::type> {
  using NumericType = typename std::conditional<sizeof(T) < sizeof(int32_t),
                                                int32_t,
                                                int64_t>::type;
  using ResultType = Argument<NumericType>;
  static ResultType Make(const char* name, T value) {
    return ResultType(name, static_cast<NumericType>(value));
  }
};

template <typename T>
inline typename ArgumentMaker<T>::ResultType MakeArgument(const char* name,
                                                          const T& value) {
  return ArgumentMaker<T>::Make(name, value);
}

// Sets up the Writer API to use |buffer| as destination for
// incoming trace records.
void StartTracing(mx::vmo current,
                  mx::vmo next,
                  const std::vector<std::string>& categories);

// Returns true iff:
//   * the tracer has been initialized by a call to InitializeTracer
//   * and tracing is active
//   * and at least one category in |categories| has been enabled
bool IsTracingEnabled(const char* categories);

// Tears down the Writer API and frees up all allocated resources.
void StopTracing();

template <typename... Args>
inline void TraceDurationBegin(const char* name,
                               const char* cat,
                               Args&&... args) {
  if (auto payload = WriteEventRecord(
          TraceEventType::kDurationBegin, name, cat, sizeof...(Args),
          SizeArguments(std::forward<Args>(args)...))) {
    payload.WriteValues(std::forward<Args>(args)...);
  }
}

template <typename... Args>
inline void TraceDurationEnd(const char* name,
                             const char* cat,
                             Args&&... args) {
  if (auto payload = WriteEventRecord(
          TraceEventType::kDurationEnd, name, cat, sizeof...(Args),
          SizeArguments(std::forward<Args>(args)...))) {
    payload.WriteValues(std::forward<Args>(args)...);
  }
}

template <typename... Args>
inline void TraceAsyncBegin(const char* name,
                            const char* cat,
                            uint64_t id,
                            Args&&... args) {
  if (auto payload = WriteEventRecord(
          TraceEventType::kAsyncStart, name, cat, sizeof...(Args),
          sizeof(id) + SizeArguments(std::forward<Args>(args)...))) {
    payload.WriteValues(std::forward<Args>(args)...).Write(id);
  }
}

template <typename... Args>
inline void TraceAsyncInstant(const char* name,
                              const char* cat,
                              uint64_t id,
                              Args&&... args) {
  if (auto payload = WriteEventRecord(
          TraceEventType::kAsyncInstant, name, cat, sizeof...(Args),
          sizeof(id) + SizeArguments(std::forward<Args>(args)...))) {
    payload.WriteValues(std::forward<Args>(args)...).Write(id);
  }
}

template <typename... Args>
inline void TraceAsyncEnd(const char* name,
                          const char* cat,
                          uint64_t id,
                          Args&&... args) {
  if (auto payload = WriteEventRecord(
          TraceEventType::kAsyncEnd, name, cat, sizeof...(Args),
          sizeof(id) + SizeArguments(std::forward<Args>(args)...))) {
    payload.WriteValues(std::forward<Args>(args)...).Write(id);
  }
}

}  // namespace internal
}  // namespace tracing

#endif  // APPS_TRACING_LIB_TRACE_INTERNAL_WRITER_H_
