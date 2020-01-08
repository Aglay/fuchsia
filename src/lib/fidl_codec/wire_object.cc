// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "wire_object.h"

#include <iomanip>
#include <iostream>
#include <memory>
#include <vector>

#include "src/lib/fidl_codec/colors.h"
#include "src/lib/fidl_codec/display_handle.h"
#include "src/lib/fidl_codec/json_visitor.h"
#include "src/lib/fidl_codec/library_loader.h"
#include "src/lib/fidl_codec/visitor.h"
#include "src/lib/fidl_codec/wire_types.h"
#include "src/lib/fxl/logging.h"

namespace fidl_codec {

constexpr char kInvalid[] = "invalid";

const Colors WithoutColors("", "", "", "", "", "");
const Colors WithColors(/*new_reset=*/"\u001b[0m", /*new_red=*/"\u001b[31m",
                        /*new_green=*/"\u001b[32m", /*new_blue=*/"\u001b[34m",
                        /*new_white_on_magenta=*/"\u001b[45m\u001b[37m",
                        /*new_yellow_background=*/"\u001b[103m");

void Value::Visit(Visitor* visitor) const { visitor->VisitValue(this); }

void InvalidValue::Visit(Visitor* visitor) const { visitor->VisitInvalidValue(this); }

void NullValue::Visit(Visitor* visitor) const { visitor->VisitNullValue(this); }

int RawValue::DisplaySize(int /*remaining_size*/) const {
  return (data_.size() == 0) ? 0 : static_cast<int>(data_.size()) * 3 - 1;
}

void RawValue::PrettyPrint(std::ostream& os, const Colors& /*colors*/,
                           const fidl_message_header_t* /*header*/,
                           std::string_view /*line_header*/, int /*tabs*/, int /*remaining_size*/,
                           int /*max_line_size*/) const {
  if (data_.size() == 0) {
    return;
  }
  size_t buffer_size = data_.size() * 3;
  std::vector<char> buffer(buffer_size);
  for (size_t i = 0; i < data_.size(); ++i) {
    if (i != 0) {
      buffer[i * 3 - 1] = ' ';
    }
    snprintf(buffer.data() + (i * 3), 4, "%02x", data_[i]);
  }
  os << buffer.data();
}

void RawValue::Visit(Visitor* visitor) const { visitor->VisitRawValue(this); }

template <>
void NumericValue<uint8_t>::Visit(Visitor* visitor) const {
  visitor->VisitU8Value(this);
}
template <>
void NumericValue<uint16_t>::Visit(Visitor* visitor) const {
  visitor->VisitU16Value(this);
}
template <>
void NumericValue<uint32_t>::Visit(Visitor* visitor) const {
  visitor->VisitU32Value(this);
}
template <>
void NumericValue<uint64_t>::Visit(Visitor* visitor) const {
  visitor->VisitU64Value(this);
}
template <>
void NumericValue<int8_t>::Visit(Visitor* visitor) const {
  visitor->VisitI8Value(this);
}
template <>
void NumericValue<int16_t>::Visit(Visitor* visitor) const {
  visitor->VisitI16Value(this);
}
template <>
void NumericValue<int32_t>::Visit(Visitor* visitor) const {
  visitor->VisitI32Value(this);
}
template <>
void NumericValue<int64_t>::Visit(Visitor* visitor) const {
  visitor->VisitI64Value(this);
}
template <>
void NumericValue<float>::Visit(Visitor* visitor) const {
  visitor->VisitF32Value(this);
}
template <>
void NumericValue<double>::Visit(Visitor* visitor) const {
  visitor->VisitF64Value(this);
}

int StringValue::DisplaySize(int /*remaining_size*/) const {
  return static_cast<int>(string_.size()) + 2;  // The two quotes.
}

void StringValue::PrettyPrint(std::ostream& os, const Colors& colors,
                              const fidl_message_header_t* /*header*/,
                              std::string_view /*line_header*/, int /*tabs*/,
                              int /*remaining_size*/, int /*max_line_size*/) const {
  os << colors.red << '"' << string_ << '"' << colors.reset;
}

void StringValue::Visit(Visitor* visitor) const { visitor->VisitStringValue(this); }

int BoolValue::DisplaySize(int /*remaining_size*/) const {
  constexpr int kTrueSize = 4;
  constexpr int kFalseSize = 5;
  return value_ ? kTrueSize : kFalseSize;
}

void BoolValue::PrettyPrint(std::ostream& os, const Colors& colors,
                            const fidl_message_header_t* /*header*/,
                            std::string_view /*line_header*/, int /*tabs*/, int /*remaining_size*/,
                            int /*max_line_size*/) const {
  os << colors.blue << (value_ ? "true" : "false") << colors.reset;
}

void BoolValue::Visit(Visitor* visitor) const { visitor->VisitBoolValue(this); }

int StructValue::DisplaySize(int remaining_size) const {
  int size = 0;
  for (const auto& member : struct_definition_.members()) {
    auto it = fields_.find(member.get());
    if (it == fields_.end())
      continue;
    // Two characters for the separator ("{ " or ", ") and three characters for
    // equal (" = ").
    constexpr int kExtraSize = 5;
    size += static_cast<int>(member->name().size()) + kExtraSize;
    // Two characters for ": ".
    size += static_cast<int>(member->type()->Name().size()) + 2;
    size += it->second->DisplaySize(remaining_size - size);
    if (size > remaining_size) {
      return size;
    }
  }
  // Two characters for the closing brace (" }").
  size += 2;
  return size;
}

void StructValue::PrettyPrint(std::ostream& os, const Colors& colors,
                              const fidl_message_header_t* header, std::string_view line_header,
                              int tabs, int remaining_size, int max_line_size) const {
  if (fields_.empty()) {
    os << "{}";
  } else if (DisplaySize(remaining_size) + static_cast<int>(line_header.size()) <= remaining_size) {
    const char* separator = "{ ";
    for (const auto& member : struct_definition_.members()) {
      auto it = fields_.find(member.get());
      if (it == fields_.end())
        continue;
      os << separator << member->name() << ": " << colors.green << member->type()->Name()
         << colors.reset << " = ";
      it->second->PrettyPrint(os, colors, header, line_header, tabs + 1, max_line_size,
                              max_line_size);
      separator = ", ";
    }
    os << " }";
  } else {
    os << "{\n";
    for (const auto& member : struct_definition_.members()) {
      auto it = fields_.find(member.get());
      if (it == fields_.end())
        continue;
      int size = (tabs + 1) * kTabSize + static_cast<int>(member->name().size());
      os << line_header << std::string((tabs + 1) * kTabSize, ' ') << member->name();
      std::string type_name = member->type()->Name();
      // Two characters for ": ", three characters for " = ".
      os << ": " << colors.green << type_name << colors.reset << " = ";
      size += static_cast<int>(type_name.size()) + 2 + 3;
      it->second->PrettyPrint(os, colors, header, line_header, tabs + 1, max_line_size - size,
                              max_line_size);
      os << "\n";
    }
    os << line_header << std::string(tabs * kTabSize, ' ') << '}';
  }
}

void StructValue::Visit(Visitor* visitor) const { visitor->VisitStructValue(this); }

void StructValue::ExtractJson(rapidjson::Document::AllocatorType& allocator,
                              rapidjson::Value& result) const {
  JsonVisitor visitor(&result, &allocator);

  Visit(&visitor);
}

bool TableValue::AddMember(std::string_view name, std::unique_ptr<Value> value) {
  const TableMember* member = table_definition_.GetMember(name);
  if (member == nullptr) {
    return false;
  }
  AddMember(member, std::move(value));
  return true;
}

int TableValue::DisplaySize(int remaining_size) const {
  int size = 0;
  for (const auto& member : table_definition_.members()) {
    if ((member != nullptr) && !member->reserved()) {
      auto it = members_.find(member.get());
      if ((it == members_.end()) || it->second->IsNull())
        continue;
      // Two characters for the separator ("{ " or ", "), three characters for " = ".
      size += static_cast<int>(member->name().size()) + 2 + 3;
      // Two characters for ": ".
      size += static_cast<int>(member->type()->Name().size()) + 2;
      size += it->second->DisplaySize(remaining_size - size);
      if (size > remaining_size) {
        return size;
      }
    }
  }
  // Two characters for the closing brace (" }").
  size += 2;
  return size;
}

void TableValue::PrettyPrint(std::ostream& os, const Colors& colors,
                             const fidl_message_header_t* header, std::string_view line_header,
                             int tabs, int remaining_size, int max_line_size) const {
  int display_size = DisplaySize(remaining_size);
  if (display_size == 2) {
    os << "{}";
  } else if (display_size + static_cast<int>(line_header.size()) <= remaining_size) {
    const char* separator = "{ ";
    for (const auto& member : table_definition_.members()) {
      if ((member != nullptr) && !member->reserved()) {
        auto it = members_.find(member.get());
        if ((it == members_.end()) || it->second->IsNull())
          continue;
        os << separator << member->name() << ": " << colors.green << member->type()->Name()
           << colors.reset << " = ";
        separator = ", ";
        it->second->PrettyPrint(os, colors, header, line_header, tabs + 1, max_line_size,
                                max_line_size);
      }
    }
    os << " }";
  } else {
    os << "{\n";
    for (const auto& member : table_definition_.members()) {
      if ((member != nullptr) && !member->reserved()) {
        auto it = members_.find(member.get());
        if ((it == members_.end()) || it->second->IsNull())
          continue;
        int size = (tabs + 1) * kTabSize + static_cast<int>(member->name().size());
        os << line_header << std::string((tabs + 1) * kTabSize, ' ') << member->name();
        std::string type_name = member->type()->Name();
        // Two characters for ": ", three characters for " = ".
        size += static_cast<int>(type_name.size()) + 2 + 3;
        os << ": " << colors.green << type_name << colors.reset << " = ";
        it->second->PrettyPrint(os, colors, header, line_header, tabs + 1, max_line_size - size,
                                max_line_size);
        os << "\n";
      }
    }
    os << line_header << std::string(tabs * kTabSize, ' ') << '}';
  }
}

void TableValue::Visit(Visitor* visitor) const { visitor->VisitTableValue(this); }

int UnionValue::DisplaySize(int remaining_size) const {
  // Two characters for the opening brace ("{ ") + three characters for equal
  // (" = ") and two characters for the closing brace (" }").
  constexpr int kExtraSize = 7;
  int size = static_cast<int>(member_.name().size()) + kExtraSize;
  // Two characters for ": ".
  size += static_cast<int>(member_.type()->Name().size()) + 2;
  size += value_->DisplaySize(remaining_size - size);
  return size;
}

void UnionValue::PrettyPrint(std::ostream& os, const Colors& colors,
                             const fidl_message_header_t* header, std::string_view line_header,
                             int tabs, int remaining_size, int max_line_size) const {
  if (header != nullptr) {
    os << (fidl_should_decode_union_from_xunion(header) ? "v1!" : "v0!");
  }
  if (DisplaySize(remaining_size) + static_cast<int>(line_header.size()) <= remaining_size) {
    // Two characters for the opening brace ("{ ") + three characters for equal
    // (" = ") and two characters for the closing brace (" }").
    constexpr int kExtraSize = 7;
    int size = static_cast<int>(member_.name().size()) + kExtraSize;
    os << "{ " << member_.name();
    std::string type_name = member_.type()->Name();
    // Two characters for ": ".
    size += static_cast<int>(type_name.size()) + 2;
    os << ": " << colors.green << type_name << colors.reset << " = ";
    value_->PrettyPrint(os, colors, header, line_header, tabs + 1, max_line_size - size,
                        max_line_size);
    os << " }";
  } else {
    os << "{\n";
    // Three characters for " = ".
    int size = (tabs + 1) * kTabSize + static_cast<int>(member_.name().size()) + 3;
    os << line_header << std::string((tabs + 1) * kTabSize, ' ') << member_.name();
    std::string type_name = member_.type()->Name();
    // Two characters for ": ".
    size += static_cast<int>(type_name.size()) + 2;
    os << ": " << colors.green << type_name << colors.reset << " = ";
    value_->PrettyPrint(os, colors, header, line_header, tabs + 1, max_line_size - size,
                        max_line_size);
    os << '\n';
    os << line_header << std::string(tabs * kTabSize, ' ') << "}";
  }
}

void UnionValue::Visit(Visitor* visitor) const { visitor->VisitUnionValue(this); }

int VectorValue::DisplaySize(int remaining_size) const {
  if (values_.empty()) {
    return 2;  // The two brackets.
  }
  if (is_string_) {
    return static_cast<int>(values_.size() + 2);  // The string and the two quotes.
  }
  int size = 0;
  for (const auto& value : values_) {
    // Two characters for the separator ("[ " or ", ").
    size += value->DisplaySize(remaining_size - size) + 2;
    if (size > remaining_size) {
      return size;
    }
  }
  // Two characters for the closing bracket (" ]").
  size += 2;
  return size;
}

void VectorValue::PrettyPrint(std::ostream& os, const Colors& colors,
                              const fidl_message_header_t* header, std::string_view line_header,
                              int tabs, int remaining_size, int max_line_size) const {
  if (values_.empty()) {
    os << "[]";
  } else if (is_string_) {
    if (has_new_line_) {
      os << "[\n";
      bool needs_header = true;
      for (const auto& value : values_) {
        if (needs_header) {
          os << line_header << std::string((tabs + 1) * kTabSize, ' ');
          needs_header = false;
        }
        uint8_t uvalue = value->GetUint8Value();
        os << uvalue;
        if (uvalue == '\n') {
          needs_header = true;
        }
      }
      if (!needs_header) {
        os << '\n';
      }
      os << line_header << std::string(tabs * kTabSize, ' ') << ']';
    } else {
      os << '"';
      for (const auto& value : values_) {
        os << value->GetUint8Value();
      }
      os << '"';
    }
  } else if (DisplaySize(remaining_size) + static_cast<int>(line_header.size()) <= remaining_size) {
    const char* separator = "[ ";
    for (const auto& value : values_) {
      os << separator;
      separator = ", ";
      value->PrettyPrint(os, colors, header, line_header, tabs + 1, max_line_size, max_line_size);
    }
    os << " ]";
  } else {
    os << "[\n";
    int size = 0;
    for (const auto& value : values_) {
      int value_size = value->DisplaySize(max_line_size - size);
      if (size == 0) {
        os << line_header << std::string((tabs + 1) * kTabSize, ' ');
        size = (tabs + 1) * kTabSize;
      } else if (value_size + 3 > max_line_size - size) {
        os << "\n";
        os << line_header << std::string((tabs + 1) * kTabSize, ' ');
        size = (tabs + 1) * kTabSize;
      } else {
        os << ", ";
        size += 2;
      }
      value->PrettyPrint(os, colors, header, line_header, tabs + 1, max_line_size - size,
                         max_line_size);
      size += value_size;
    }
    os << '\n';
    os << line_header << std::string(tabs * kTabSize, ' ') << ']';
  }
}

void VectorValue::Visit(Visitor* visitor) const { visitor->VisitVectorValue(this); }

int EnumValue::DisplaySize(int /*remaining_size*/) const {
  if (!data_) {
    return strlen(kInvalid);
  }
  return enum_definition_.GetNameFromBytes(data_->data()).size();
}

void EnumValue::PrettyPrint(std::ostream& os, const Colors& colors,
                            const fidl_message_header_t* /*header*/,
                            std::string_view /*line_header*/, int /*tabs*/, int /*remaining_size*/,
                            int /*max_line_size*/) const {
  if (!data_) {
    os << colors.red << kInvalid << colors.reset;
  } else {
    os << colors.blue << enum_definition_.GetNameFromBytes(data_->data()) << colors.reset;
  }
}

void EnumValue::Visit(Visitor* visitor) const { visitor->VisitEnumValue(this); }

int BitsValue::DisplaySize(int /*remaining_size*/) const {
  if (!data_) {
    return strlen(kInvalid);
  }
  return bits_definition_.GetNameFromBytes(data_->data()).size();
}

void BitsValue::PrettyPrint(std::ostream& os, const Colors& colors,
                            const fidl_message_header_t* /*header*/,
                            std::string_view /*line_header*/, int /*tabs*/, int /*remaining_size*/,
                            int /*max_line_size*/) const {
  if (!data_) {
    os << colors.red << kInvalid << colors.reset;
  } else {
    os << colors.blue << bits_definition_.GetNameFromBytes(data_->data()) << colors.reset;
  }
}

void BitsValue::Visit(Visitor* visitor) const { visitor->VisitBitsValue(this); }

int HandleValue::DisplaySize(int /*remaining_size*/) const {
  return std::to_string(handle_.handle).size();
}

void HandleValue::PrettyPrint(std::ostream& os, const Colors& colors,
                              const fidl_message_header_t* /*header*/,
                              std::string_view /*line_header*/, int /*tabs*/,
                              int /*remaining_size*/, int /*max_line_size*/) const {
  DisplayHandle(colors, handle_, os);
}

void HandleValue::Visit(Visitor* visitor) const { visitor->VisitHandleValue(this); }

}  // namespace fidl_codec
