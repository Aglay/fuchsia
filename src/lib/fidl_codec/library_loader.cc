// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "library_loader.h"

#include <src/lib/fxl/logging.h>

#include "rapidjson/error/en.h"
#include "src/lib/fidl_codec/wire_object.h"
#include "src/lib/fidl_codec/wire_types.h"

// See library_loader.h for details.

namespace fidl_codec {

Enum::Enum(Library* enclosing_library, const rapidjson::Value& value)
    : enclosing_library_(enclosing_library), value_(value) {}

Enum::~Enum() = default;

void Enum::DecodeTypes() {
  if (decoded_) {
    return;
  }
  decoded_ = true;
  name_ = enclosing_library_->ExtractString(value_, "enum", "<unknown>", "name");
  type_ = enclosing_library_->ExtractScalarType(value_, "enum", name_, "type", 0);

  if (!value_.HasMember("members")) {
    enclosing_library_->FieldNotFound("enum", name_, "members");
  }

  size_ = type_->InlineSize();
}

std::string Enum::GetNameFromBytes(const uint8_t* bytes) const {
  if (value_.HasMember("members")) {
    for (auto& member : value_["members"].GetArray()) {
      if (member.HasMember("value") && member["value"].HasMember("literal") &&
          (type_->ValueEquals(bytes, size_, member["value"]["literal"]))) {
        if (!member.HasMember("name")) {
          return "<unknown>";
        }
        return member["name"].GetString();
      }
    }
  }
  return "<unknown>";
}

UnionMember::UnionMember(Library* enclosing_library, const rapidjson::Value& value, bool for_xunion)
    : name_(enclosing_library->ExtractString(value, "union member", "<unknown>", "name")),
      offset_(enclosing_library->ExtractUint64(value, "union member", name_, "offset")),
      size_(enclosing_library->ExtractUint64(value, "union member", name_, "size")),
      ordinal_(for_xunion
                   ? enclosing_library->ExtractUint32(value, "union member", name_, "ordinal")
                   : (value.HasMember("xunion_ordinal")
                          ? enclosing_library->ExtractUint32(value, "union member", name_,
                                                             "xunion_ordinal")
                          : 0)),
      type_(enclosing_library->ExtractType(value, "union member", name_, "type", size_)) {}

UnionMember::~UnionMember() = default;

Union::Union(Library* enclosing_library, const rapidjson::Value& value)
    : enclosing_library_(enclosing_library), value_(value) {}

void Union::DecodeTypes(bool for_xunion) {
  if (decoded_) {
    return;
  }
  decoded_ = true;
  name_ = enclosing_library_->ExtractString(value_, "union", "<unknown>", "name");
  alignment_ = enclosing_library_->ExtractUint64(value_, "union", name_, "alignment");
  size_ = enclosing_library_->ExtractUint64(value_, "union", name_, "size");

  if (!value_.HasMember("members")) {
    enclosing_library_->FieldNotFound("union", name_, "members");
  } else {
    auto member_arr = value_["members"].GetArray();
    members_.reserve(member_arr.Size());
    for (auto& member : member_arr) {
      members_.push_back(std::make_unique<UnionMember>(enclosing_library_, member, for_xunion));
    }
  }
}

const UnionMember* Union::MemberWithTag(uint32_t tag) const {
  if (tag >= members_.size()) {
    return nullptr;
  }
  return members_[tag].get();
}

const UnionMember* Union::MemberWithOrdinal(Ordinal32 ordinal) const {
  for (const auto& member : members_) {
    if (member->ordinal() == ordinal) {
      return member.get();
    }
  }
  return nullptr;
}

std::unique_ptr<UnionField> Union::DecodeUnion(MessageDecoder* decoder, std::string_view name,
                                               const Type* type, uint64_t offset,
                                               bool nullable) const {
  std::unique_ptr<UnionField> result = std::make_unique<UnionField>(name, type, *this);
  if (nullable) {
    result->DecodeNullable(decoder, offset, size_);
  } else {
    result->DecodeAt(decoder, offset);
  }
  return result;
}

std::unique_ptr<XUnionField> Union::DecodeXUnion(MessageDecoder* decoder, std::string_view name,
                                                 const Type* type, uint64_t offset,
                                                 bool nullable) const {
  uint32_t ordinal = 0;
  if (decoder->GetValueAt(offset, &ordinal)) {
    if ((ordinal == 0) && !nullable) {
      FXL_LOG(ERROR) << "null envelope for a non nullable extensible union";
    }
  }
  offset += sizeof(uint64_t);  // Skips ordinal + padding.

  std::unique_ptr<XUnionField> result = std::make_unique<XUnionField>(name, type, *this);

  std::unique_ptr<EnvelopeField> envelope;
  const UnionMember* member = MemberWithOrdinal(ordinal);
  if (member == nullptr) {
    std::string key_name = std::string("unknown$") + std::to_string(ordinal);
    envelope = std::make_unique<EnvelopeField>(key_name, nullptr);
  } else {
    envelope = std::make_unique<EnvelopeField>(member->name(), member->type());
  }
  envelope->DecodeAt(decoder, offset);
  result->set_field(std::move(envelope));
  return result;
}

StructMember::StructMember(Library* enclosing_library, const rapidjson::Value& value)
    : name_(enclosing_library->ExtractString(value, "struct member", "<unknown>", "name")),
      offset_(enclosing_library->ExtractUint64(value, "struct member", name_, "offset")),
      size_(enclosing_library->ExtractUint64(value, "struct member", name_, "size")),
      type_(enclosing_library->ExtractType(value, "struct member", name_, "type", size_)) {}

StructMember::~StructMember() = default;

Struct::Struct(Library* enclosing_library, const rapidjson::Value& value)
    : enclosing_library_(enclosing_library), value_(value) {}

void Struct::DecodeStructTypes() {
  if (decoded_) {
    return;
  }
  DecodeTypes("struct", "size", "members");
}

void Struct::DecodeRequestTypes() {
  if (decoded_) {
    return;
  }
  DecodeTypes("request", "maybe_request_size", "maybe_request");
}

void Struct::DecodeResponseTypes() {
  if (decoded_) {
    return;
  }
  DecodeTypes("response", "maybe_response_size", "maybe_response");
}

std::unique_ptr<Object> Struct::DecodeObject(MessageDecoder* decoder, std::string_view name,
                                             const Type* type, uint64_t offset,
                                             bool nullable) const {
  std::unique_ptr<Object> result = std::make_unique<Object>(name, type, *this);
  if (nullable) {
    result->DecodeNullable(decoder, offset, size_);
  } else {
    result->DecodeAt(decoder, offset);
  }
  return result;
}

void Struct::DecodeTypes(std::string_view container_name, const char* size_name,
                         const char* member_name) {
  FXL_DCHECK(!decoded_);
  decoded_ = true;
  name_ = enclosing_library_->ExtractString(value_, container_name, "<unknown>", "name");
  size_ = enclosing_library_->ExtractUint64(value_, container_name, name_, size_name);

  if (!value_.HasMember(member_name)) {
    enclosing_library_->FieldNotFound(container_name, name_, member_name);
  } else {
    auto member_arr = value_[member_name].GetArray();
    members_.reserve(member_arr.Size());
    for (auto& member : member_arr) {
      members_.push_back(std::make_unique<StructMember>(enclosing_library_, member));
    }
  }
}

TableMember::TableMember(Library* enclosing_library, const rapidjson::Value& value)
    : reserved_(enclosing_library->ExtractBool(value, "table member", "<unknown>", "reserved")),
      name_(reserved_
                ? "<reserved>"
                : enclosing_library->ExtractString(value, "table member", "<unknown>", "name")),
      ordinal_(enclosing_library->ExtractUint32(value, "table member", name_, "ordinal")),
      size_(reserved_ ? 0 : enclosing_library->ExtractUint64(value, "table member", name_, "size")),
      type_(reserved_
                ? std::make_unique<RawType>(0)
                : enclosing_library->ExtractType(value, "table member", name_, "type", size_)) {}

TableMember::~TableMember() = default;

Table::Table(Library* enclosing_library, const rapidjson::Value& value)
    : enclosing_library_(enclosing_library), value_(value) {}

Table::~Table() = default;

void Table::DecodeTypes() {
  if (decoded_) {
    return;
  }
  decoded_ = true;
  name_ = enclosing_library_->ExtractString(value_, "table", "<unknown>", "name");
  size_ = enclosing_library_->ExtractUint64(value_, "table", name_, "size");

  unknown_member_type_ = std::make_unique<RawType>(size_);

  if (!value_.HasMember("members")) {
    enclosing_library_->FieldNotFound("table", name_, "members");
  } else {
    auto member_arr = value_["members"].GetArray();
    Ordinal32 max_ordinal = 0;
    for (auto& member : member_arr) {
      backing_members_.push_back(std::make_unique<TableMember>(enclosing_library_, member));
      max_ordinal = std::max(max_ordinal, backing_members_.back()->ordinal());
    }

    // There is one element in this array for each possible ordinal value.  The
    // array is dense, so there are unlikely to be gaps.
    members_.resize(max_ordinal + 1, nullptr);
    for (const auto& backing_member : backing_members_) {
      members_[backing_member->ordinal()] = backing_member.get();
    }
  }
}

InterfaceMethod::InterfaceMethod(const Interface& interface, const rapidjson::Value& value)
    : enclosing_interface_(interface),
      name_(interface.enclosing_library()->ExtractString(value, "method", "<unknown>", "name")),
      // TODO(FIDL-524): Step 4, i.e. both ord and gen are prepared by fidlc for
      // direct consumption by the bindings.
      ordinal_(interface.enclosing_library()->ExtractUint64(value, "method", name_, "ordinal")),
      old_ordinal_(interface.enclosing_library()->ExtractUint64(value, "method", name_,
                                                                "generated_ordinal")),
      is_composed_(
          interface.enclosing_library()->ExtractBool(value, "method", name_, "is_composed")) {
  if (interface.enclosing_library()->ExtractBool(value, "method", name_, "has_request")) {
    request_ = std::unique_ptr<Struct>(new Struct(interface.enclosing_library(), value));
  }

  if (interface.enclosing_library()->ExtractBool(value, "method", name_, "has_response")) {
    response_ = std::unique_ptr<Struct>(new Struct(interface.enclosing_library(), value));
  }
}

std::string InterfaceMethod::fully_qualified_name() const {
  std::string fqn(enclosing_interface_.name());
  fqn.append(".");
  fqn.append(name());
  return fqn;
}

bool Interface::GetMethodByFullName(const std::string& name,
                                    const InterfaceMethod** method_ptr) const {
  for (const auto& method : methods()) {
    if (method->fully_qualified_name() == name) {
      *method_ptr = method.get();
      return true;
    }
  }
  return false;
}

Library::Library(LibraryLoader* enclosing_loader, rapidjson::Document& document,
                 std::map<Ordinal64, std::unique_ptr<std::vector<const InterfaceMethod*>>>& index)
    : enclosing_loader_(enclosing_loader), backing_document_(std::move(document)) {
  auto interfaces_array = backing_document_["interface_declarations"].GetArray();
  interfaces_.reserve(interfaces_array.Size());

  for (auto& decl : interfaces_array) {
    interfaces_.emplace_back(new Interface(this, decl));
    interfaces_.back()->AddMethodsToIndex(index);
  }
}

Library::~Library() { enclosing_loader()->Delete(this); }

void Library::DecodeTypes() {
  if (decoded_) {
    return;
  }
  decoded_ = true;
  name_ = ExtractString(backing_document_, "library", "<unknown>", "name");

  if (!backing_document_.HasMember("enum_declarations")) {
    FieldNotFound("library", name_, "enum_declarations");
  } else {
    for (auto& enu : backing_document_["enum_declarations"].GetArray()) {
      enums_.emplace(std::piecewise_construct, std::forward_as_tuple(enu["name"].GetString()),
                     std::forward_as_tuple(new Enum(this, enu)));
    }
  }

  if (!backing_document_.HasMember("struct_declarations")) {
    FieldNotFound("library", name_, "struct_declarations");
  } else {
    for (auto& str : backing_document_["struct_declarations"].GetArray()) {
      structs_.emplace(std::piecewise_construct, std::forward_as_tuple(str["name"].GetString()),
                       std::forward_as_tuple(new Struct(this, str)));
    }
  }

  if (!backing_document_.HasMember("table_declarations")) {
    FieldNotFound("library", name_, "table_declarations");
  } else {
    for (auto& tab : backing_document_["table_declarations"].GetArray()) {
      tables_.emplace(std::piecewise_construct, std::forward_as_tuple(tab["name"].GetString()),
                      std::forward_as_tuple(new Table(this, tab)));
    }
  }

  if (!backing_document_.HasMember("union_declarations")) {
    FieldNotFound("library", name_, "union_declarations");
  } else {
    for (auto& uni : backing_document_["union_declarations"].GetArray()) {
      unions_.emplace(std::piecewise_construct, std::forward_as_tuple(uni["name"].GetString()),
                      std::forward_as_tuple(new Union(this, uni)));
    }
  }

  if (!backing_document_.HasMember("xunion_declarations")) {
    FieldNotFound("library", name_, "xunion_declarations");
  } else {
    for (auto& xuni : backing_document_["xunion_declarations"].GetArray()) {
      xunions_.emplace(std::piecewise_construct, std::forward_as_tuple(xuni["name"].GetString()),
                       std::forward_as_tuple(new XUnion(this, xuni)));
    }
  }
}

bool Library::DecodeAll() {
  DecodeTypes();
  for (const auto& tmp : structs_) {
    tmp.second->DecodeStructTypes();
  }
  for (const auto& tmp : enums_) {
    tmp.second->DecodeTypes();
  }
  for (const auto& tmp : tables_) {
    tmp.second->DecodeTypes();
  }
  for (const auto& tmp : unions_) {
    tmp.second->DecodeUnionTypes();
  }
  for (const auto& tmp : xunions_) {
    tmp.second->DecodeXunionTypes();
  }
  for (const auto& interface : interfaces_) {
    for (const auto& method : interface->methods()) {
      method->request();
      method->response();
    }
  }
  return !has_errors_;
}

std::unique_ptr<Type> Library::TypeFromIdentifier(bool is_nullable, std::string& identifier,
                                                  size_t inline_size) {
  auto str = structs_.find(identifier);
  if (str != structs_.end()) {
    str->second->DecodeStructTypes();
    std::unique_ptr<Type> type(new StructType(std::ref(*str->second), is_nullable));
    return type;
  }
  auto enu = enums_.find(identifier);
  if (enu != enums_.end()) {
    enu->second->DecodeTypes();
    return std::make_unique<EnumType>(std::ref(*enu->second));
  }
  auto tab = tables_.find(identifier);
  if (tab != tables_.end()) {
    tab->second->DecodeTypes();
    return std::make_unique<TableType>(std::ref(*tab->second));
  }
  auto uni = unions_.find(identifier);
  if (uni != unions_.end()) {
    uni->second->DecodeUnionTypes();
    return std::make_unique<UnionType>(std::ref(*uni->second), is_nullable);
  }
  auto xuni = xunions_.find(identifier);
  if (xuni != xunions_.end()) {
    // Note: XUnion and nullable XUnion are encoded in the same way
    xuni->second->DecodeXunionTypes();
    return std::make_unique<XUnionType>(std::ref(*xuni->second), is_nullable);
  }
  const Interface* ifc;
  if (GetInterfaceByName(identifier, &ifc)) {
    return std::make_unique<HandleType>();
  }
  return std::make_unique<RawType>(inline_size);
}

bool Library::GetInterfaceByName(const std::string& name, const Interface** ptr) const {
  for (const auto& interface : interfaces()) {
    if (interface->name() == name) {
      *ptr = interface.get();
      return true;
    }
  }
  return false;
}

bool Library::ExtractBool(const rapidjson::Value& value, std::string_view container_type,
                          std::string_view container_name, const char* field_name) {
  if (!value.HasMember(field_name)) {
    FieldNotFound(container_type, container_name, field_name);
    return false;
  }
  return value[field_name].GetBool();
}

std::string Library::ExtractString(const rapidjson::Value& value, std::string_view container_type,
                                   std::string_view container_name, const char* field_name) {
  if (!value.HasMember(field_name)) {
    FieldNotFound(container_type, container_name, field_name);
    return "<unknown>";
  }
  return value["name"].GetString();
}

uint64_t Library::ExtractUint64(const rapidjson::Value& value, std::string_view container_type,
                                std::string_view container_name, const char* field_name) {
  if (!value.HasMember(field_name)) {
    FieldNotFound(container_type, container_name, field_name);
    return 0;
  }
  return std::strtoll(value[field_name].GetString(), nullptr, kDecimalBase);
}

uint32_t Library::ExtractUint32(const rapidjson::Value& value, std::string_view container_type,
                                std::string_view container_name, const char* field_name) {
  if (!value.HasMember(field_name)) {
    FieldNotFound(container_type, container_name, field_name);
    return 0;
  }
  return std::strtoll(value[field_name].GetString(), nullptr, kDecimalBase);
}

std::unique_ptr<Type> Library::ExtractScalarType(const rapidjson::Value& value,
                                                 std::string_view container_type,
                                                 std::string_view container_name,
                                                 const char* field_name, uint64_t size) {
  if (!value.HasMember(field_name)) {
    FieldNotFound(container_type, container_name, field_name);
    return std::make_unique<RawType>(size);
  }
  return Type::ScalarTypeFromName(value[field_name].GetString(), size);
}

std::unique_ptr<Type> Library::ExtractType(const rapidjson::Value& value,
                                           std::string_view container_type,
                                           std::string_view container_name, const char* field_name,
                                           uint64_t size) {
  if (!value.HasMember(field_name)) {
    FieldNotFound(container_type, container_name, field_name);
    return std::make_unique<RawType>(size);
  }
  return Type::GetType(enclosing_loader(), value[field_name], size);
}

void Library::FieldNotFound(std::string_view container_type, std::string_view container_name,
                            const char* field_name) {
  has_errors_ = true;
  FXL_LOG(ERROR) << "File " << name() << " field '" << field_name << "' missing for "
                 << container_type << ' ' << container_name;
}

LibraryLoader::LibraryLoader(std::vector<std::unique_ptr<std::istream>>* library_streams,
                             LibraryReadError* err) {
  AddAll(library_streams, err);
}

bool LibraryLoader::AddAll(std::vector<std::unique_ptr<std::istream>>* library_streams,
                           LibraryReadError* err) {
  bool ok = true;
  err->value = LibraryReadError::kOk;
  // Go backwards through the streams; we refuse to load the same library twice, and the last one
  // wins.
  for (auto i = library_streams->rbegin(); i != library_streams->rend(); ++i) {
    Add(&(*i), err);
    if (err->value != LibraryReadError::kOk) {
      ok = false;
    }
  }
  return ok;
}

bool LibraryLoader::DecodeAll() {
  bool ok = true;
  for (const auto& representation : representations_) {
    Library* library = representation.second.get();
    if (!library->DecodeAll()) {
      ok = false;
    }
  }
  return ok;
}

void LibraryLoader::Add(std::unique_ptr<std::istream>* library_stream, LibraryReadError* err) {
  err->value = LibraryReadError::kOk;
  std::string ir(std::istreambuf_iterator<char>(**library_stream), {});
  if ((*library_stream)->fail()) {
    err->value = LibraryReadError ::kIoError;
    return;
  }
  Add(ir, err);
  if (err->value != LibraryReadError::kOk) {
    FXL_LOG(ERROR) << "JSON parse error: " << rapidjson::GetParseError_En(err->parse_result.Code())
                   << " at offset " << err->parse_result.Offset();
    return;
  }
}

}  // namespace fidl_codec
