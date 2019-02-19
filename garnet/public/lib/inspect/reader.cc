// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lib/inspect/reader.h"

#include "lib/fit/bridge.h"

namespace inspect {

ObjectReader::ObjectReader(
    fidl::InterfaceHandle<fuchsia::inspect::Inspect> inspect_handle)
    : state_(std::make_shared<internal::ObjectReaderState>()) {
  state_->inspect_ptr_.Bind(std::move(inspect_handle));
}

fit::promise<fuchsia::inspect::Object> ObjectReader::Read() const {
  fit::bridge<fuchsia::inspect::Object> bridge;
  state_->inspect_ptr_->ReadData(bridge.completer.bind());
  return bridge.consumer.promise();
}

fit::promise<ChildNameVector> ObjectReader::ListChildren() const {
  fit::bridge<ChildNameVector> bridge;
  state_->inspect_ptr_->ListChildren(bridge.completer.bind());
  return bridge.consumer.promise();
}

fit::promise<ObjectReader> ObjectReader::OpenChild(
    std::string child_name) const {
  fuchsia::inspect::InspectPtr child_ptr;

  fit::bridge<bool> bridge;

  state_->inspect_ptr_->OpenChild(child_name, child_ptr.NewRequest(),
                                  bridge.completer.bind());

  ObjectReader reader(child_ptr.Unbind());
  return bridge.consumer.promise().and_then(
      [ret = std::move(reader)](
          bool success) mutable -> fit::result<ObjectReader> {
        if (success) {
          return fit::ok(ObjectReader(std::move(ret)));
        } else {
          return fit::error();
        }
      });
}

fit::promise<std::vector<ObjectReader>> ObjectReader::OpenChildren() const {
  return ListChildren()
      .and_then([reader = *this](const ChildNameVector& children) {
        std::vector<fit::promise<ObjectReader>> opens;
        for (const auto& child_name : *children) {
          opens.emplace_back(reader.OpenChild(child_name));
        }
        return fit::join_promise_vector(std::move(opens));
      })
      .and_then([](std::vector<fit::result<ObjectReader>>& objects) {
        std::vector<ObjectReader> result;
        for (auto& obj : objects) {
          if (obj.is_ok()) {
            result.emplace_back(obj.take_value());
          }
        }
        return fit::ok(std::move(result));
      });
}

ObjectHierarchy::ObjectHierarchy(fuchsia::inspect::Object object,
                                 std::vector<ObjectHierarchy> children)
    : object_(std::move(object)), children_(std::move(children)) {}

const ObjectHierarchy* ObjectHierarchy::GetByPath(
    std::vector<std::string> path) const {
  const ObjectHierarchy* current = this;
  auto path_it = path.begin();

  while (current && path_it != path.end()) {
    const ObjectHierarchy* next = nullptr;
    for (const auto& obj : current->children_) {
      if (obj.object().name == *path_it) {
        next = &obj;
        break;
      }
    }
    current = next;
    ++path_it;
  }
  return current;
}

fit::promise<ObjectHierarchy> ObjectHierarchy::Make(ObjectReader reader,
                                                    int depth) {
  auto reader_promise = reader.Read();
  if (depth == 0) {
    return reader_promise.and_then([reader](fuchsia::inspect::Object& obj) {
      return fit::ok(ObjectHierarchy(std::move(obj), {}));
    });
  } else {
    auto children_promise =
        reader.OpenChildren()
            .and_then(
                [depth](std::vector<ObjectReader>& result)
                    -> fit::promise<std::vector<fit::result<ObjectHierarchy>>> {
                  std::vector<fit::promise<ObjectHierarchy>> children;
                  for (auto& reader : result) {
                    children.emplace_back(Make(std::move(reader), depth - 1));
                  }

                  return fit::join_promise_vector(std::move(children));
                })
            .and_then([](std::vector<fit::result<ObjectHierarchy>>& result) {
              std::vector<ObjectHierarchy> children;
              for (auto& res : result) {
                if (res.is_ok()) {
                  children.emplace_back(res.take_value());
                }
              }
              return fit::ok(std::move(children));
            });

    return fit::join_promises(std::move(reader_promise),
                              std::move(children_promise))
        .and_then(
            [reader](std::tuple<fit::result<fuchsia::inspect::Object>,
                                fit::result<std::vector<ObjectHierarchy>>>&
                         result) mutable -> fit::result<ObjectHierarchy> {
              return fit::ok(ObjectHierarchy(std::get<0>(result).take_value(),
                                             std::get<1>(result).take_value()));
            });
  }
}

ObjectHierarchy ObjectHierarchy::Make(const inspect::Object& object,
                                      int depth) {
  return Make(object.object_dir().object(), depth);
}

ObjectHierarchy ObjectHierarchy::Make(
    std::shared_ptr<component::Object> object_root, int depth) {
  auto obj = object_root->ToFidl();
  if (depth == 0) {
    return ObjectHierarchy(std::move(obj), {});
  } else {
    std::vector<ObjectHierarchy> children;
    auto child_names = object_root->GetChildren();
    for (auto& child_name : *child_names) {
      auto child_obj = object_root->GetChild(child_name);
      if (child_obj) {
        children.emplace_back(Make(child_obj, depth - 1));
      }
    }
    return ObjectHierarchy(std::move(obj), std::move(children));
  }
}

}  // namespace inspect
