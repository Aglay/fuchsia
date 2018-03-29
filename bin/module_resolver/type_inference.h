// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string>
#include <vector>

#include <fuchsia/cpp/modular.h>
#include "lib/async/cpp/operation.h"

namespace modular {

class NounTypeInferenceHelper {
 public:
  NounTypeInferenceHelper(modular::EntityResolverPtr entity_resolver);
  ~NounTypeInferenceHelper();

  // Returns a list of types represented in |noun_constraint|. Chooses the
  // correct process for type extraction based on the type of Noun.
  void GetNounTypes(
      const modular::ResolverNounConstraint& noun_constraint,
      const std::function<void(std::vector<std::string>)>& result_callback);

 private:
  class GetNounTypesCall;

  modular::EntityResolverPtr entity_resolver_;
  modular::OperationCollection operation_collection_;

  FXL_DISALLOW_COPY_AND_ASSIGN(NounTypeInferenceHelper);
};

}  // namespace modular
