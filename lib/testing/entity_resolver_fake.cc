// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/lib/testing/entity_resolver_fake.h"

namespace modular {

class EntityResolverFake::EntityImpl : Entity {
 public:
  EntityImpl(std::map<std::string, std::string> types_and_data)
      : types_and_data_(types_and_data) {}

  void Connect(f1dl::InterfaceRequest<Entity> request) {
    bindings_.AddBinding(this, std::move(request));
  }

 private:
  // |Entity|
  void GetTypes(const GetTypesCallback& callback) override {
    f1dl::Array<f1dl::String> types;
    for (const auto& entry : types_and_data_) {
      types.push_back(entry.first);
    }
    callback(std::move(types));
  }

  // |Entity|
  void GetData(const f1dl::String& type,
               const GetDataCallback& callback) override {
    auto it = types_and_data_.find(type);
    if (it == types_and_data_.end()) {
      callback(nullptr);
      return;
    }
    callback(it->second);
  }

  std::map<std::string, std::string> types_and_data_;

  f1dl::BindingSet<Entity> bindings_;
};

EntityResolverFake::EntityResolverFake() = default;
EntityResolverFake::~EntityResolverFake() = default;

void EntityResolverFake::Connect(
    f1dl::InterfaceRequest<EntityResolver> request) {
  bindings_.AddBinding(this, std::move(request));
}

// Returns an Entity reference that will resolve to an Entity.
// |types_and_data| is a map of data type to data bytes.
f1dl::String EntityResolverFake::AddEntity(
    std::map<std::string, std::string> types_and_data) {
  const std::string id = std::to_string(next_entity_id_++);

  auto entity = std::make_unique<EntityImpl>(std::move(types_and_data));
  entities_.emplace(id, std::move(entity));
  return id;
}

void EntityResolverFake::ResolveEntity(
    const f1dl::String& entity_reference,
    f1dl::InterfaceRequest<Entity> entity_request) {
  auto it = entities_.find(entity_reference);
  if (it == entities_.end()) {
    return;  // |entity_request| is reset here.
  }

  it->second->Connect(std::move(entity_request));
}

}  // namespace modular
