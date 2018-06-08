// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_SUGGESTION_CPP_FORMATTING_H_
#define LIB_SUGGESTION_CPP_FORMATTING_H_

#include <fuchsia/modular/cpp/fidl.h>

namespace modular {

std::ostream& operator<<(std::ostream& os,
                         const fuchsia::modular::SuggestionDisplay& o);
std::ostream& operator<<(std::ostream& os,
                         const fuchsia::modular::Suggestion& o);

}  // namespace modular

#endif  // LIB_SUGGESTION_CPP_FORMATTING_H_
