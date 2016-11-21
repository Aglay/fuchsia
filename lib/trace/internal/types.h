// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APPS_TRACING_LIB_TRACE_INTERNAL_TYPES_H_
#define APPS_TRACING_LIB_TRACE_INTERNAL_TYPES_H_

namespace tracing {
namespace internal {

inline size_t Pad(size_t size) {
  return size + ((8 - (size & 7)) & 7);
}

// Enumerates all known record types.
enum class RecordType {
  kMetadata = 0,
  kInitialization = 1,
  kString = 2,
  kThread = 3,
  kEvent = 4
};

// Enumerates all known types of arguments.
//
// Extend at end.
enum class ArgumentType {
  kNull = 0,
  kInt32 = 1,
  kInt64 = 2,
  kDouble = 3,
  kString = 4,
  kPointer = 5,
  kKernelObjectId = 6
};

// TraceEventType enumerates all well-known
// types of trace events.
enum class TraceEventType {
  kDurationBegin = 1,
  kDurationEnd = 2,
  kAsyncStart = 3,
  kAsyncInstant = 4,
  kAsyncEnd = 5
};

struct StringRefFields {
  static constexpr uint16_t kEmpty = 0;
  static constexpr uint16_t kInvalidIndex = 0;
  static constexpr uint16_t kInlineFlag = 0x8000;
  static constexpr uint16_t kLengthMask = 0x7fff;
  static constexpr size_t kMaxLength = 0x7fff;
  static constexpr uint16_t kMaxIndex = 0x7fff;
};

struct ThreadRefFields {
  static constexpr uint16_t kInline = 0;
  static constexpr uint16_t kMaxIndex = 0xff;
};

template <size_t begin, size_t end>
struct Field {
  static_assert(begin < sizeof(uint64_t) * 8, "begin is out of bounds");
  static_assert(end < sizeof(uint64_t) * 8, "end is out of bounds");
  static_assert(begin <= end, "begin must not be larger than end");

  static constexpr uint64_t kMask = (uint64_t(1) << (end - begin + 1)) - 1;

  static uint64_t Make(uint64_t value) { return value << begin; }

  template <typename U>
  static U Get(uint64_t word) {
    return static_cast<U>((word >> begin) & kMask);
  }

  static void Set(uint64_t& word, uint64_t value) {
    word = (word & ~(kMask << begin)) | (value << begin);
  }
};

using ArgumentHeader = uint64_t;

struct ArgumentFields {
  using Type = Field<0, 3>;
  using ArgumentSize = Field<4, 15>;
  using NameRef = Field<16, 31>;
};

struct Int32ArgumentFields : ArgumentFields {
  using Value = Field<32, 63>;
};

struct StringArgumentFields : ArgumentFields {
  using Index = Field<32, 47>;
};

using RecordHeader = uint64_t;

struct RecordFields {
  static constexpr size_t kMaxRecordSizeWords = 0xfff;
  static constexpr size_t kMaxRecordSizeBytes =
      kMaxRecordSizeWords * sizeof(uint64_t);

  using Type = Field<0, 3>;
  using RecordSize = Field<4, 15>;
};

using InitializationRecordFields = RecordFields;

struct StringRecordFields : RecordFields {
  using StringIndex = Field<16, 30>;
  using StringLength = Field<32, 46>;
};

struct ThreadRecordFields : RecordFields {
  using ThreadIndex = Field<16, 23>;
};

struct EventRecordFields : RecordFields {
  using EventType = Field<16, 19>;
  using ArgumentCount = Field<20, 23>;
  using ThreadRef = Field<24, 31>;
  using CategoryStringRef = Field<32, 47>;
  using NameStringRef = Field<48, 63>;
};

}  // namspace internal
}  // namepsace tracing

#endif  // APPS_TRACING_LIB_TRACE_INTERNAL_TYPES_H_
