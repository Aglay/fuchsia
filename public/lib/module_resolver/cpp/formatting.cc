// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lib/module_resolver/cpp/formatting.h"

namespace modular {

std::ostream& operator<<(std::ostream& os, const Daisy& daisy) {
  os << "{ verb: " << daisy.verb << ", nouns: [" << std::endl;
  for (auto it = daisy.nouns.begin(); it != daisy.nouns.end(); ++it) {
    os << "    " << (*it)->name << ": " << (*it)->noun << "," << std::endl;
  }
  os << "  ] }";
  return os;
}

std::ostream& operator<<(std::ostream& os, const Noun& noun) {
  if (noun.is_json()) {
    os << noun.get_json();
  } else if (noun.is_entity_reference()) {
    os << "[ref: " << noun.get_entity_reference() << "]";
  } else if (noun.is_entity_type()) {
    for (const auto& type : noun.get_entity_type()) {
      os << type << ", ";
    }
  }
  return os;
}

}  // namespace modular
