// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_SUGGESTION_CPP_FORMATTING_H_
#define LIB_SUGGESTION_CPP_FORMATTING_H_

#include "lib/fidl/cpp/formatting.h"
#include "lib/suggestion/fidl/suggestion_provider.fidl.h"

namespace maxwell {

std::ostream& operator<<(std::ostream& os, const SuggestionDisplay& o);
std::ostream& operator<<(std::ostream& os, const Suggestion& o);

}  // namespace maxwell

#endif  // LIB_SUGGESTION_CPP_FORMATTING_H_
