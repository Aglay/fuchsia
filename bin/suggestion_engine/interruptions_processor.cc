// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/suggestion_engine/interruptions_processor.h"

#include "peridot/bin/suggestion_engine/ranking_feature.h"

namespace modular {

InterruptionsProcessor::InterruptionsProcessor() = default;
InterruptionsProcessor::~InterruptionsProcessor() = default;

void InterruptionsProcessor::RegisterListener(
    fidl::InterfaceHandle<InterruptionListener> listener) {
  listeners_.AddInterfacePtr(listener.Bind());
}

bool InterruptionsProcessor::ConsiderSuggestion(
    const SuggestionPrototype& prototype) {
  // TODO(jwnichols): Implement a real interruption pipeline here
  if (IsInterruption(prototype)) {
    for (auto& listener : listeners_.ptrs()) {
      DispatchInterruption(listener->get(), prototype);
    }
    return true;
  }
  return false;
}

bool InterruptionsProcessor::IsInterruption(
    const SuggestionPrototype& prototype) {
  return prototype.proposal.display.annoyance != AnnoyanceType::NONE;
}

void InterruptionsProcessor::DispatchInterruption(
    InterruptionListener* const listener,
    const SuggestionPrototype& prototype) {
  Suggestion suggestion = CreateSuggestion(prototype);
  suggestion.confidence = kMaxConfidence;
  listener->OnInterrupt(std::move(suggestion));
}

}  // namespace modular
