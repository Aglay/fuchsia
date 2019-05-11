// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "wire_types.h"

#include <src/lib/fxl/logging.h>
#include <zircon/fidl.h>

#include "rapidjson/error/en.h"
#include "tools/fidlcat/lib/library_loader.h"

// See wire_types.h for details.

namespace fidlcat {

namespace {

// Objects are 8-byte aligned.  This adds 7 to the current offset and masks out
// the last three bits.
inline const uint8_t* AlignToNextWordBoundary(const uint8_t* offset) {
  const uintptr_t val = reinterpret_cast<const uintptr_t>(offset);
  return reinterpret_cast<uint8_t*>((val + 7) & (~7));
}

}  // namespace

bool ObjectTracker::RunCallbacksFrom(Marker& marker) {
  marker.AdvanceBytesTo(AlignToNextWordBoundary(marker.byte_pos()));
  if (!marker.is_valid()) {
    return false;
  }
  // We can't just iterate over the callbacks, because the callbacks may add
  // more callbacks.
  while (!callbacks_.empty()) {
    auto callback = std::move(callbacks_.front());
    callback(marker);
    if (!marker.is_valid()) {
      return false;
    }
    callbacks_.erase(callbacks_.begin());
  }
  return true;
}

void ObjectTracker::MessageEnqueue(
    ValueGeneratingCallback&& callback, rapidjson::Value& target_object,
    rapidjson::Document::AllocatorType& allocator) {
  callbacks_.push_back([this, cb = std::move(callback), &target_object,
                        &allocator](Marker& marker) {
    cb(this, marker, target_object, allocator);
  });
}

void ObjectTracker::ObjectEnqueue(
    const std::string& key, ValueGeneratingCallback&& callback,
    rapidjson::Value& target_object,
    rapidjson::Document::AllocatorType& allocator) {
  FXL_DCHECK(callback) << "No callback for object";
  callbacks_.push_back([this, cb = std::move(callback), &target_object,
                        key_string = key, &allocator](Marker& marker) {
    rapidjson::Value key;
    key.SetString(key_string.c_str(), allocator);

    rapidjson::Value& object =
        target_object.AddMember(key, rapidjson::Value(), allocator);
    if (!cb(this, marker, object[key_string.c_str()], allocator)) {
      target_object.RemoveMember(key_string);
    }
  });
}

void ObjectTracker::ArrayEnqueue(
    ValueGeneratingCallback&& callback, rapidjson::Value& target_array,
    rapidjson::Document::AllocatorType& allocator) {
  callbacks_.push_back([this, cb = std::move(callback), &target_array,
                        &allocator](Marker& marker) {
    rapidjson::Value element;
    cb(this, marker, element, allocator);
    if (!marker.is_valid()) {
      return;
    }
    target_array.PushBack(element, allocator);
  });
}

void Marker::AdvanceBytesBy(size_t amount) {
  const uint8_t* pos = byte_pos_ + amount;
  AdvanceBytesTo(pos);
}

void Marker::AdvanceBytesTo(const uint8_t* pos) { byte_pos_ = pos; }

void Marker::AdvanceHandlesBy(size_t amount) {
  const zx_handle_t* pos = handle_pos_ + amount;
  AdvanceHandlesTo(pos);
}

void Marker::AdvanceHandlesTo(const zx_handle_t* pos) { handle_pos_ = pos; }

bool Marker::is_valid() const {
  return (end_byte_pos_ == nullptr || byte_pos_ <= end_byte_pos_) &&
         (end_byte_pos_ == nullptr || handle_pos_ <= end_handle_pos_);
}

std::string Marker::ToString() const {
  std::ostringstream ss;
  ss << "[" << (void*)byte_pos_ << "(" << (void*)end_byte_pos_ << "), "
     << (void*)handle_pos_ << "(" << (void*)end_handle_pos_ << ")";
  return ss.str();
}

namespace {

ValueGeneratingCallback NullCallback(bool keep_null) {
  return [keep_null](ObjectTracker* tracker, Marker& marker,
                     rapidjson::Value& value,
                     rapidjson::Document::AllocatorType& allocator) {
    value.SetNull();
    return keep_null;
  };
}

}  // namespace

// Prints out raw bytes as a C string of hex pairs ("af b0 1e...").  Useful for
// debugging / unknown data.
Marker UnknownType::GetValueCallback(Marker marker, size_t length,
                                     ObjectTracker* tracker,
                                     ValueGeneratingCallback& callback) const {
  const uint8_t* bytes = marker.byte_pos();
  marker.AdvanceBytesBy(length);
  if (marker.is_valid()) {
    callback = [length, bytes](ObjectTracker* tracker, Marker marker,
                               rapidjson::Value& value,
                               rapidjson::Document::AllocatorType& allocator) {
      size_t size = length * 3 + 1;
      char output[size];
      for (size_t i = 0; i < length; i++) {
        snprintf(output + (i * 3), 4, "%02x ", bytes[i]);
      }
      output[size - 2] = '\0';
      value.SetString(output, size, allocator);
      return true;
    };
  }
  return marker;
}

bool Type::ValueEquals(Marker marker, size_t length,
                       const rapidjson::Value& value) const {
  FXL_LOG(FATAL) << "Equality operator for type not implemented";
  return false;
}

size_t Type::InlineSize() const {
  FXL_LOG(FATAL) << "Size for type not implemented";
  return 0;
}

Marker StringType::GetValueCallback(Marker marker, size_t length,
                                    ObjectTracker* tracker,
                                    ValueGeneratingCallback& callback) const {
  const uint8_t* bytes = marker.byte_pos();
  marker.AdvanceBytesBy(length);
  if (!marker.is_valid()) {
    return marker;
  }
  // Strings: First 8 bytes is length
  uint64_t string_length = internal::MemoryFrom<uint64_t>(bytes);
  // next 8 bytes are 0 if the string is null, and 0xffffffff otherwise.
  bool is_null = bytes[sizeof(uint64_t)] == 0x0;
  callback = [is_null, string_length](
                 ObjectTracker* tracker, Marker& marker,
                 rapidjson::Value& value,
                 rapidjson::Document::AllocatorType& allocator) {
    if (is_null) {
      value.SetString("(null)", allocator);
      return true;
    }
    const uint8_t* bytes = marker.byte_pos();
    marker.AdvanceBytesTo(
        AlignToNextWordBoundary(marker.byte_pos() + string_length));
    if (marker.is_valid()) {
      // everything after that is the string.
      value.SetString(reinterpret_cast<const char*>(bytes), string_length,
                      allocator);
    }
    return true;
  };
  return marker;
}

Marker BoolType::GetValueCallback(Marker marker, size_t length,
                                  ObjectTracker* tracker,
                                  ValueGeneratingCallback& callback) const {
  const uint8_t* bytes = marker.byte_pos();
  marker.AdvanceBytesBy(sizeof(bool));
  if (marker.is_valid()) {
    callback = [val = *bytes](ObjectTracker* tracker, Marker& marker,
                              rapidjson::Value& value,
                              rapidjson::Document::AllocatorType& allocator) {
      // assert that length == 1
      if (val) {
        value.SetString("true", allocator);
      } else {
        value.SetString("false", allocator);
      }
      return true;
    };
  }
  return marker;
}

Marker StructType::GetValueCallback(Marker marker, size_t length,
                                    ObjectTracker* tracker,
                                    ValueGeneratingCallback& callback) const {
  const Struct& str = struct_;
  callback = [str, mp = marker](ObjectTracker* tracker, Marker& marker,
                                rapidjson::Value& value,
                                rapidjson::Document::AllocatorType& allocator) {
    value.SetObject();
    Marker inline_marker = mp;
    Marker prev_marker = inline_marker;
    for (auto& member : str.members()) {
      std::unique_ptr<Type> member_type = member.GetType();
      ValueGeneratingCallback value_callback;
      Marker value_marker(inline_marker.byte_pos() + member.offset(),
                          prev_marker.handle_pos(), tracker->end());
      if (!value_marker.is_valid()) {
        marker = value_marker;
        return false;
      }

      prev_marker = member_type->GetValueCallback(value_marker, member.size(),
                                                  tracker, value_callback);
      if (!prev_marker.is_valid()) {
        marker = value_marker;
        return false;
      }

      tracker->ObjectEnqueue(std::string(member.name()),
                             std::move(value_callback), value, allocator);
    }
    return true;
  };
  marker.AdvanceBytesBy(length);
  return marker;
}

size_t StructType::InlineSize() const { return struct_.size(); }

// Convenience class to access an Envelope embedded in a uint8_t array.  The
// format is:
// [ uint32_t num_bytes, uint32_t num_handles, uint64_t pointer ]
class Envelope {
 public:
  Envelope(const uint8_t* ptr) : ptr_(ptr) {}

  uint32_t num_bytes() const { return internal::MemoryFrom<uint32_t>(ptr_); }

  uint32_t num_handles() const {
    return internal::MemoryFrom<uint32_t>(ptr_ + sizeof(uint32_t));
  }

  uint64_t pointer() const {
    return internal::MemoryFrom<uint64_t>(ptr_ + (2 * sizeof(uint32_t)));
  }

  const uint8_t* pointer_offset() {
    return ptr_ + sizeof(uint32_t) + sizeof(uint32_t);
  }

  std::string ToString() {
    std::ostringstream oss;
    oss << "(" << num_bytes() << ", " << num_handles() << ", " << pointer()
        << ")";
    return oss.str();
  }

  static constexpr size_t INLINE_SIZE =
      sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint64_t);

 private:
  const uint8_t* ptr_;
};

class EnvelopeType : public Type {
 public:
  explicit EnvelopeType(Type* target_type) : target_type_(target_type) {}

  virtual Marker GetValueCallback(
      Marker marker, size_t length, ObjectTracker* tracker,
      ValueGeneratingCallback& callback) const override {
    Envelope envelope(marker.byte_pos());

    // An envelope is a byte count, a handle count, and a pointer.  The referent
    // of the pointer is very likely to be known, but it may not be.  In the
    // cases where it isn't, we want to know the length, which can only be
    // provided by examining the envelope.  This is why there is a has-a
    // relationship between the EnvelopeType and a pointer type, rather than an
    // is-a relationship.
    std::unique_ptr<PointerType> pointer_type;
    if (target_type_ == nullptr) {
      pointer_type.reset(
          new PointerType(new UnknownType(envelope.num_bytes()), false));
    } else {
      pointer_type.reset(new PointerType(target_type_, false));
    }
    marker.AdvanceBytesTo(envelope.pointer_offset());
    if (!marker.is_valid()) {
      return marker;
    }
    ValueGeneratingCallback pointer_callback;
    marker = pointer_type->GetValueCallback(marker, sizeof(uint64_t), tracker,
                                            pointer_callback);
    callback = [envelope, cb = std::move(pointer_callback)](
                   ObjectTracker* tracker, Marker& marker,
                   rapidjson::Value& value,
                   rapidjson::Document::AllocatorType& allocator) {
      Marker tmp = marker;
      // Always advance bytes and handles by the envelope-provided
      // values, regardless of what the type might say (or not say)
      marker.AdvanceBytesBy(envelope.num_bytes());
      marker.AdvanceHandlesBy(envelope.num_handles());
      return cb(tracker, tmp, value, allocator);
    };
    return marker;
  }

  virtual size_t InlineSize() const override {
    return Envelope::INLINE_SIZE;
  }

 private:
  std::shared_ptr<Type> target_type_;
};

Marker TableType::GetValueCallback(Marker marker, size_t length,
                                   ObjectTracker* tracker,
                                   ValueGeneratingCallback& callback) const {
  const uint8_t* bytes = marker.byte_pos();
  marker.AdvanceBytesBy(length);
  if (!marker.is_valid()) {
    return marker;
  }

  uint64_t max_ordinal = internal::MemoryFrom<uint64_t>(bytes);
  uint64_t data = internal::MemoryFrom<uint64_t>(bytes + sizeof(uint64_t));

  // data is only allowed to be FIDL_ALLOC_PRESENT.
  if (data != FIDL_ALLOC_PRESENT) {
    // TODO: add "name" to the type, print it here instead of "object".
    FXL_LOG(INFO) << "Illegally encoded table " << max_ordinal << " " << data;
    return marker;
  }

  const Table& tab = table_;
  callback = [max_ordinal, &tab](
                 ObjectTracker* tracker, Marker& marker,
                 rapidjson::Value& value,
                 rapidjson::Document::AllocatorType& allocator) {
    value.SetObject();
    auto& members = tab.members();

    for (size_t ordinal = 1; ordinal <= max_ordinal; ordinal++) {
      std::unique_ptr<Type> target_type;
      std::string name;
      if (ordinal < members.size() && members[ordinal] != nullptr) {
        target_type = members[ordinal]->GetType();
        name = members[ordinal]->name();
      } else {
        name = "unknown$";
        name.append(std::to_string(ordinal));
      }
      EnvelopeType type(target_type.release());
      ValueGeneratingCallback value_callback;
      marker = type.GetValueCallback(marker, type.InlineSize(), tracker,
                                     value_callback);
      if (!marker.is_valid()) {
        return false;
      }
      tracker->ObjectEnqueue(name, std::move(value_callback), value, allocator);
    }
    return true;
  };

  return marker;
}

size_t TableType::InlineSize() const { return table_.size(); }

UnionType::UnionType(const Union& uni) : union_(uni) {}

Marker UnionType::GetValueCallback(Marker marker, size_t length,
                                   ObjectTracker* tracker,
                                   ValueGeneratingCallback& callback) const {
  const Union& uni = union_;
  callback = [uni, mp = marker](ObjectTracker* tracker, Marker& marker,
                                rapidjson::Value& value,
                                rapidjson::Document::AllocatorType& allocator) {
    value.SetObject();
    Marker inline_marker = mp;
    const uint8_t* final_pos = inline_marker.byte_pos() + uni.size();

    // Read tag
    const uint8_t* bytes = inline_marker.byte_pos();
    uint32_t tag = internal::MemoryFrom<uint32_t>(bytes);
    inline_marker.AdvanceBytesBy(uni.alignment());
    if (!inline_marker.is_valid()) {
      marker = inline_marker;
      return false;
    }

    // Determine member type and get appropriate callback
    const UnionMember& member = uni.MemberWithTag(tag);
    std::unique_ptr<Type> member_type = member.GetType();
    ValueGeneratingCallback raw_value_callback;
    inline_marker = member_type->GetValueCallback(inline_marker, member.size(),
                                                  tracker, raw_value_callback);
    ValueGeneratingCallback value_callback =
        [rvc = std::move(raw_value_callback), final_pos, tracker,
         inline_marker](ObjectTracker* ignored, Marker& tracker_marker,
                        rapidjson::Value& value,
                        rapidjson::Document::AllocatorType& allocator) {
          Marker marker = inline_marker;
          rvc(tracker, marker, value, allocator);

          // Advance marker's pos to the real end, which may or may not be the
          // end of the value.
          marker.AdvanceBytesTo(final_pos);
          if (!marker.is_valid()) {
            tracker_marker = marker;
            return false;
          }
          return true;
        };

    tracker->ObjectEnqueue(std::string(member.name()),
                           std::move(value_callback), value, allocator);
    return true;
  };

  marker.AdvanceBytesBy(length);
  return marker;
}

size_t UnionType::InlineSize() const { return union_.size(); }

XUnionType::XUnionType(const XUnion& uni, bool is_nullable)
    : xunion_(uni), is_nullable_(is_nullable) {}

Marker XUnionType::GetValueCallback(Marker marker, size_t length,
                                    ObjectTracker* tracker,
                                    ValueGeneratingCallback& callback) const {
  const uint8_t* bytes = marker.byte_pos();
  // Advance by the size of the ordinal + padding.
  marker.AdvanceBytesBy(sizeof(uint64_t));
  if (!marker.is_valid()) {
    return marker;
  }

  uint32_t ordinal = internal::MemoryFrom<uint32_t>(bytes);
  if (ordinal == 0) {
    if (!is_nullable_) {
      FXL_LOG(WARNING) << "Encoding error: found null value in non-nullable "
                          "xunion.  This is likely a bug in the FIDL binding, "
                          "so contact the FIDL binding authors.";
    }
    Envelope envelope(marker.byte_pos());
    FXL_CHECK(envelope.num_bytes() == 0 && envelope.num_handles() == 0 &&
              envelope.pointer() == 0)
        << "Undefined ordinal in xunion without empty envelope.";
    callback = NullCallback(true);
    marker.AdvanceBytesBy(Envelope::INLINE_SIZE);
    return marker;
  }
  std::unique_ptr<Type> target_type;
  std::string member_name;
  for (auto& member : xunion_.members()) {
    std::optional<Ordinal> maybe_target_ordinal = member.ordinal();
    if (!maybe_target_ordinal) {
      continue;
    }
    Ordinal target_ordinal = *maybe_target_ordinal;
    if (target_ordinal == ordinal) {
      target_type = member.GetType();
      member_name = member.name();
      break;
    }
  }
  if (target_type == nullptr) {
    // Need to figure out what to do with this...
    member_name = "unknown$";
    member_name.append(std::to_string(ordinal));
  }

  EnvelopeType type(target_type.release());
  ValueGeneratingCallback value_callback;
  marker =
      type.GetValueCallback(marker, type.InlineSize(), tracker, value_callback);
  callback = [member_name, value_callback = std::move(value_callback)](
                 ObjectTracker* tracker, Marker& marker,
                 rapidjson::Value& value,
                 rapidjson::Document::AllocatorType& allocator) mutable {
    value.SetObject();
    tracker->ObjectEnqueue(member_name, std::move(value_callback), value,
                           allocator);
    return true;
  };

  return marker;
}

size_t XUnionType::InlineSize() const { return xunion_.size(); }

PointerType::PointerType(Type* target_type, bool keep_null)
    : target_type_(target_type), keep_null_(keep_null) {}

PointerType::PointerType(std::shared_ptr<Type> target_type, bool keep_null)
    : target_type_(target_type), keep_null_(keep_null) {}

Marker PointerType::GetValueCallback(Marker marker, size_t length,
                                     ObjectTracker* tracker,
                                     ValueGeneratingCallback& callback) const {
  // Check the intptr and maybe return a callback that does nothing but set a
  // null value.
  const uint8_t* bytes = marker.byte_pos();
  uintptr_t data;
  marker.AdvanceBytesBy(sizeof(data));
  if (!marker.is_valid()) {
    return marker;
  }
  data = internal::MemoryFrom<uint64_t>(bytes);
  if (data == FIDL_ALLOC_ABSENT) {
    callback = NullCallback(keep_null_);
    return marker;
  }

  if (data != FIDL_ALLOC_PRESENT) {
    // TODO: add "name" to the type, print it here instead of "object".
    FXL_LOG(INFO) << "Illegally encoded object";
  }

  // If the intptr is non-null, return a callback that will track an out of line
  // object of the wrapped type.
  callback = [target_type = target_type_](
                 ObjectTracker* tracker, Marker& marker,
                 rapidjson::Value& value,
                 rapidjson::Document::AllocatorType& allocator) {
    ValueGeneratingCallback callback;
    ObjectTracker local_tracker(tracker->end());
    Marker val = target_type->GetValueCallback(
        marker, target_type->InlineSize(), &local_tracker, callback);
    if (!val.is_valid()) {
      marker = val;
      return false;
    }
    bool retval = callback(&local_tracker, marker, value, allocator);
    local_tracker.RunCallbacksFrom(val);
    if (!val.is_valid()) {
      marker = val;
      return false;
    }
    return retval;
  };
  return marker;
}

ElementSequenceType::ElementSequenceType(std::unique_ptr<Type>&& component_type)
    : component_type_(std::move(component_type)) {
  FXL_DCHECK(component_type_.get() != nullptr);
}

ElementSequenceType::ElementSequenceType(std::shared_ptr<Type> component_type)
    : component_type_(component_type) {
  FXL_DCHECK(component_type_.get() != nullptr);
}

ValueGeneratingCallback ElementSequenceType::GetIteratingCallback(
    ObjectTracker* tracker, size_t count, Marker marker, size_t length) const {
  std::shared_ptr<Type> component_type = component_type_;
  return [tracker, component_type, count, captured_marker = marker, length](
             ObjectTracker* ignored, Marker& inline_marker,
             rapidjson::Value& value,
             rapidjson::Document::AllocatorType& allocator) {
    value.SetArray();
    Marker marker = captured_marker;
    for (uint32_t i = 0; i < count; i++) {
      ValueGeneratingCallback value_callback;
      marker = component_type->GetValueCallback(marker, length / count, tracker,
                                                value_callback);
      if (!marker.is_valid()) {
        inline_marker = marker;
        return false;
      }
      tracker->ArrayEnqueue(std::move(value_callback), value, allocator);
    }
    return true;
  };
}

ArrayType::ArrayType(std::unique_ptr<Type>&& component_type, uint32_t count)
    : ElementSequenceType(std::move(component_type)), count_(count) {}

Marker ArrayType::GetValueCallback(Marker marker, size_t length,
                                   ObjectTracker* tracker,
                                   ValueGeneratingCallback& callback) const {
  callback = GetIteratingCallback(tracker, count_, marker, length);
  marker.AdvanceBytesBy(length);
  return marker;
}

VectorType::VectorType(std::unique_ptr<Type>&& component_type)
    : ElementSequenceType(std::move(component_type)) {}

VectorType::VectorType(std::shared_ptr<Type> component_type,
                       size_t element_size)
    : ElementSequenceType(component_type) {}

Marker VectorType::GetValueCallback(Marker marker, size_t length,
                                    ObjectTracker* tracker,
                                    ValueGeneratingCallback& callback) const {
  const uint8_t* bytes = marker.byte_pos();
  marker.AdvanceBytesBy(length);
  if (!marker.is_valid()) {
    return marker;
  }

  uint64_t count = internal::MemoryFrom<uint64_t>(bytes);
  uint64_t data = internal::MemoryFrom<uint64_t>(bytes + sizeof(uint64_t));
  size_t element_size = component_type_->InlineSize();
  if (data == UINTPTR_MAX) {
    VectorType vt(component_type_, element_size);
    callback = [vt, tracker, element_size, count](
                   ObjectTracker* ignored, Marker& marker,
                   rapidjson::Value& value,
                   rapidjson::Document::AllocatorType& allocator) {
      ValueGeneratingCallback value_cb =
          vt.GetIteratingCallback(tracker, count, marker, element_size * count);
      bool retval = value_cb(tracker, marker, value, allocator);
      marker.AdvanceBytesBy(element_size * count);
      return retval;
    };
  } else if (data == 0) {
    // TODO: Validate this is a nullable vector.
    callback = NullCallback(true);
  }
  return marker;
}

EnumType::EnumType(const Enum& e) : enum_(e) {}

Marker EnumType::GetValueCallback(Marker marker, size_t length,
                                  ObjectTracker* tracker,
                                  ValueGeneratingCallback& callback) const {
  std::string name = enum_.GetNameFromBytes(marker.byte_pos(), length);
  marker.AdvanceBytesBy(length);
  if (!marker.is_valid()) {
    return marker;
  }
  callback = [name](ObjectTracker* tracker, Marker& marker,
                    rapidjson::Value& value,
                    rapidjson::Document::AllocatorType& allocator) {
    value.SetString(name, allocator);
    return true;
  };
  return marker;
}

Marker HandleType::GetValueCallback(Marker marker, size_t length,
                                    ObjectTracker* tracker,
                                    ValueGeneratingCallback& callback) const {
  const uint8_t* bytes = marker.byte_pos();
  marker.AdvanceBytesBy(sizeof(zx_handle_t));
  if (!marker.is_valid()) {
    return marker;
  }
  zx_handle_t val = internal::MemoryFrom<zx_handle_t>(bytes);
  if (val == FIDL_HANDLE_PRESENT) {
    // Handle is out-of-line
    callback = [](ObjectTracker* tracker, Marker& marker,
                  rapidjson::Value& value,
                  rapidjson::Document::AllocatorType& allocator) {
      const zx_handle_t* handles = marker.handle_pos();
      marker.AdvanceHandlesBy(1);
      if (!marker.is_valid()) {
        return false;
      }
      zx_handle_t val = internal::MemoryFrom<zx_handle_t>(handles);
      value.SetString(std::to_string(val).c_str(), allocator);
      return true;
    };
  } else if (val == FIDL_HANDLE_ABSENT) {
    callback = [val](ObjectTracker* tracker, Marker& marker,
                     rapidjson::Value& value,
                     rapidjson::Document::AllocatorType& allocator) {
      value.SetString(std::to_string(val).c_str(), allocator);
      return true;
    };
  } else {
    FXL_LOG(INFO) << "Illegally encoded handle";
  }
  return marker;
}

std::unique_ptr<Type> Type::get_illegal() {
  return std::unique_ptr<Type>(new UnknownType());
}

std::unique_ptr<Type> Type::ScalarTypeFromName(const std::string& type_name) {
  static std::map<std::string, std::function<std::unique_ptr<Type>()>>
      scalar_type_map_{
          {"bool", []() { return std::make_unique<BoolType>(); }},
          {"float32", []() { return std::make_unique<Float32Type>(); }},
          {"float64", []() { return std::make_unique<Float64Type>(); }},
          {"int8", []() { return std::make_unique<Int8Type>(); }},
          {"int16", []() { return std::make_unique<Int16Type>(); }},
          {"int32", []() { return std::make_unique<Int32Type>(); }},
          {"int64", []() { return std::make_unique<Int64Type>(); }},
          {"uint8", []() { return std::make_unique<Uint8Type>(); }},
          {"uint16", []() { return std::make_unique<Uint16Type>(); }},
          {"uint32", []() { return std::make_unique<Uint32Type>(); }},
          {"uint64", []() { return std::make_unique<Uint64Type>(); }},
      };
  auto it = scalar_type_map_.find(type_name);
  if (it != scalar_type_map_.end()) {
    return it->second();
  }
  return Type::get_illegal();
}

std::unique_ptr<Type> Type::TypeFromPrimitive(const rapidjson::Value& type) {
  if (!type.HasMember("subtype")) {
    FXL_LOG(ERROR) << "Invalid type";
    return Type::get_illegal();
  }

  std::string subtype = type["subtype"].GetString();
  return ScalarTypeFromName(subtype);
}

std::unique_ptr<Type> Type::TypeFromIdentifier(const LibraryLoader& loader,
                                               const rapidjson::Value& type) {
  if (!type.HasMember("identifier")) {
    FXL_LOG(ERROR) << "Invalid type";
    return std::unique_ptr<Type>();
  }
  std::string id = type["identifier"].GetString();
  size_t split_index = id.find('/');
  std::string library_name = id.substr(0, split_index);
  const Library* library;
  if (!loader.GetLibraryFromName(library_name, &library)) {
    FXL_LOG(ERROR) << "Unknown type for identifier: " << library_name;
    // TODO: Something else here
    return std::unique_ptr<Type>();
  }

  bool is_nullable = false;
  if (type.HasMember("nullable")) {
    is_nullable = type["nullable"].GetBool();
  }
  return library->TypeFromIdentifier(is_nullable, id);
}

std::unique_ptr<Type> Type::GetType(const LibraryLoader& loader,
                                    const rapidjson::Value& type) {
  // TODO: This is creating a new type every time we need one.  That's pretty
  // inefficient.  Find a way of caching them if it becomes a problem.
  if (!type.HasMember("kind")) {
    FXL_LOG(ERROR) << "Invalid type";
    return Type::get_illegal();
  }
  std::string kind = type["kind"].GetString();
  if (kind == "array") {
    const rapidjson::Value& element_type = type["element_type"];
    uint32_t element_count =
        std::strtol(type["element_count"].GetString(), nullptr, 10);
    return std::make_unique<ArrayType>(GetType(loader, element_type),
                                       element_count);
  } else if (kind == "vector") {
    const rapidjson::Value& element_type = type["element_type"];
    return std::make_unique<VectorType>(GetType(loader, element_type));
  } else if (kind == "string") {
    return std::make_unique<StringType>();
  } else if (kind == "handle") {
    return std::make_unique<HandleType>();
  } else if (kind == "request") {
    return std::make_unique<HandleType>();
  } else if (kind == "primitive") {
    return Type::TypeFromPrimitive(type);
  } else if (kind == "identifier") {
    return Type::TypeFromIdentifier(loader, type);
  }
  FXL_LOG(ERROR) << "Invalid type " << kind;
  return get_illegal();
}

}  // namespace fidlcat
