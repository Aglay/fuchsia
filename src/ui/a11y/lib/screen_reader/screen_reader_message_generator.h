// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_A11Y_LIB_SCREEN_READER_SCREEN_READER_MESSAGE_GENERATOR_H_
#define SRC_UI_A11Y_LIB_SCREEN_READER_SCREEN_READER_MESSAGE_GENERATOR_H_

#include <fuchsia/accessibility/semantics/cpp/fidl.h>
#include <fuchsia/accessibility/tts/cpp/fidl.h>
#include <lib/zx/time.h>

#include <memory>
#include <vector>

#include "src/ui/a11y/lib/screen_reader/i18n/message_formatter.h"

namespace a11y {

// The ScreenReaderMessageGenerator creates screen reader output (node descriptions, hints, etc.),
// which is spoken to the user by a tts system. For example, a semantic node which is a button,
// with label 'ok', could be represented as: Utterance: 'ok', (with 200 ms delay) Utterance:
// 'button'.
class ScreenReaderMessageGenerator {
 public:
  // Holds an utterance and some metadata used to control how it should be spoken.
  struct UtteranceAndContext {
    // The utterance to be spoken.
    fuchsia::accessibility::tts::Utterance utterance;
    // The delay that should be introduced before this utterance is spoken.
    zx::duration delay = zx::msec(0);
  };

  // |message_formatter| is the resourses object used by this class tto retrieeve localized message
  // strings by their unique MessageId. The language used is the language loaded in
  // |message_formatter|.
  explicit ScreenReaderMessageGenerator(std::unique_ptr<i18n::MessageFormatter> message_formatter);
  virtual ~ScreenReaderMessageGenerator() = default;

  // Returns a description of the semantic node.
  virtual std::vector<UtteranceAndContext> DescribeNode(
      const fuchsia::accessibility::semantics::Node* node);

 protected:
  // Constructor for mock only.
  ScreenReaderMessageGenerator() = default;

 private:
  std::unique_ptr<i18n::MessageFormatter> message_formatter_;
};

}  // namespace a11y

#endif  // SRC_UI_A11Y_LIB_SCREEN_READER_SCREEN_READER_MESSAGE_GENERATOR_H_
