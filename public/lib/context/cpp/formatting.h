// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_CONTEXT_CPP_FORMATTING_H_
#define LIB_CONTEXT_CPP_FORMATTING_H_

#include <fuchsia/modular/cpp/fidl.h>

namespace fuchsia {
namespace modular {

std::ostream& operator<<(std::ostream& os,
                         const fuchsia::modular::FocusedState& state);
std::ostream& operator<<(std::ostream& os,
                         const fuchsia::modular::StoryMetadata& meta);
std::ostream& operator<<(std::ostream& os,
                         const fuchsia::modular::ModuleMetadata& meta);
std::ostream& operator<<(std::ostream& os,
                         const fuchsia::modular::EntityMetadata& meta);
std::ostream& operator<<(std::ostream& os,
                         const fuchsia::modular::LinkMetadata& meta);
std::ostream& operator<<(std::ostream& os,
                         const fuchsia::modular::ContextMetadata& meta);

std::ostream& operator<<(std::ostream& os,
                         const fuchsia::modular::ContextValue& value);
std::ostream& operator<<(std::ostream& os,
                         const fuchsia::modular::ContextSelector& selector);

std::ostream& operator<<(std::ostream& os,
                         const fuchsia::modular::ContextUpdate& update);
std::ostream& operator<<(std::ostream& os,
                         const fuchsia::modular::ContextQuery& query);

}  // namespace modular
}  // namespace fuchsia

#endif  // LIB_CONTEXT_CPP_FORMATTING_H_
