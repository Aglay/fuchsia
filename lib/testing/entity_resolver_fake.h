// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_LIB_TESTING_ENTITY_RESOLVER_FAKE_H_
#define PERIDOT_LIB_TESTING_ENTITY_RESOLVER_FAKE_H_

#include <map>
#include <memory>
#include <string>

#include <fuchsia/modular/cpp/fidl.h>

#include "lib/fidl/cpp/binding_set.h"
#include "lib/fidl/cpp/interface_request.h"
#include "lib/fidl/cpp/string.h"
#include "lib/fxl/macros.h"

namespace fuchsia {
namespace modular {

class EntityResolverFake : public EntityResolver {
 public:
  EntityResolverFake();
  ~EntityResolverFake() override;

  void Connect(fidl::InterfaceRequest<EntityResolver> request);

  // Returns an Entity reference that will resolve to an Entity.
  // |types_and_data| is a map of data type to data bytes.
  fidl::StringPtr AddEntity(std::map<std::string, std::string> types_and_data);

 private:
  class EntityImpl;

  void ResolveEntity(fidl::StringPtr entity_reference,
                     fidl::InterfaceRequest<Entity> entity_request) override;

  int next_entity_id_{0};
  std::map<std::string, std::unique_ptr<EntityImpl>> entities_;
  fidl::BindingSet<EntityResolver> bindings_;
};

}  // namespace modular
}  // namespace fuchsia

#endif  // PERIDOT_LIB_TESTING_ENTITY_RESOLVER_FAKE_H_
