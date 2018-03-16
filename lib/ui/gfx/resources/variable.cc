// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/lib/ui/gfx/resources/variable.h"

#include "garnet/lib/ui/gfx/util/unwrap.h"
#include "garnet/lib/ui/scenic/util/error_reporter.h"

namespace scenic {
namespace gfx {

const ResourceTypeInfo Variable::kTypeInfo = {ResourceType::kVariable,
                                              "Variable"};

Variable::Variable(Session* session, scenic::ResourceId id)
    : Resource(session, id, Variable::kTypeInfo) {}

Variable::~Variable() {}

template <ui::gfx::Value::Tag VT, typename T>
TypedVariable<VT, T>::TypedVariable(Session* session, scenic::ResourceId id)
    : Variable(session, id),
      value_()  // Initialize |value_| to its type's default value.
{}

template <ui::gfx::Value::Tag VT, typename T>
void TypedVariable<VT, T>::SetValue(T value) {
  value_ = value;
  for (auto listener : listeners_)
    listener->OnVariableValueChanged(this);
}

template <ui::gfx::Value::Tag VT, typename T>
bool TypedVariable<VT, T>::SetValue(const ui::gfx::ValuePtr& value) {
  bool success = Unwrap(value, &value_);
  FXL_DCHECK(success);
  for (auto listener : listeners_)
    listener->OnVariableValueChanged(this);
  return success;
}

template <ui::gfx::Value::Tag VT, typename T>
void TypedVariable<VT, T>::Accept(class ResourceVisitor* visitor){};

// Explicitly instantiate all the classes in Scenic that use the Variable<>
// template.
template class TypedVariable<ui::gfx::Value::Tag::VECTOR1, float>;
template class TypedVariable<ui::gfx::Value::Tag::VECTOR2, escher::vec2>;
template class TypedVariable<ui::gfx::Value::Tag::VECTOR3, escher::vec3>;
template class TypedVariable<ui::gfx::Value::Tag::VECTOR4, escher::vec4>;
template class TypedVariable<ui::gfx::Value::Tag::MATRIX4X4, escher::mat4>;
template class TypedVariable<ui::gfx::Value::Tag::QUATERNION, escher::quat>;
// template class TypedVariable<ui::gfx::Value::Tag::TRANSFORM,
// escher::Transform>;

}  // namespace gfx
}  // namespace scenic
