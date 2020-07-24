// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_A11Y_LIB_SCREEN_READER_TESTS_MOCKS_MOCK_SCREEN_READER_CONTEXT_H_
#define SRC_UI_A11Y_LIB_SCREEN_READER_TESTS_MOCKS_MOCK_SCREEN_READER_CONTEXT_H_

#include <memory>
#include <string>
#include <vector>

#include "src/ui/a11y/lib/screen_reader/focus/tests/mocks/mock_a11y_focus_manager.h"
#include "src/ui/a11y/lib/screen_reader/screen_reader_context.h"
#include "src/ui/a11y/lib/screen_reader/speaker.h"

namespace accessibility_test {

class MockScreenReaderContext : public a11y::ScreenReaderContext {
 public:
  class MockSpeaker : public a11y::Speaker {
   public:
    MockSpeaker() = default;
    ~MockSpeaker() override = default;

    // |Speaker|
    fit::promise<> SpeakNodePromise(const fuchsia::accessibility::semantics::Node* node,
                                    Options options) override;

    // |Speaker|
    fit::promise<> SpeakMessagePromise(fuchsia::accessibility::tts::Utterance utterance,
                                       Options options) override;

    // |Speaker|
    fit::promise<> CancelTts() override;

    // Returns true whether any speak request was done.
    bool ReceivedSpeak() const { return received_speak_; }

    // Returns whether speech was cancelled.
    bool ReceivedCancel() const { return received_cancel_; }

    // Returns the vector that collects all messages sent to SpeakMessagePromise().
    std::vector<std::string>& messages() { return messages_; }
    // Returns the vector that collects all node IDs to be described by SpeakPromise().
    std::vector<uint32_t>& node_ids() { return node_ids_; }

   private:
    std::vector<std::string> messages_;
    std::vector<uint32_t> node_ids_;
    bool received_speak_ = false;
    bool received_cancel_ = false;
  };

  MockScreenReaderContext();
  ~MockScreenReaderContext() override = default;

  // Pointer to the mocks, so expectations can be configured in tests.
  MockA11yFocusManager* mock_a11y_focus_manager_ptr() { return mock_a11y_focus_manager_ptr_; }
  MockSpeaker* mock_speaker_ptr() { return mock_speaker_ptr_; }

  // |ScreenReaderContext|
  a11y::A11yFocusManager* GetA11yFocusManager() override { return a11y_focus_manager_.get(); }

  // |ScreenReaderContext|
  a11y::Speaker* speaker() override { return speaker_.get(); }

 private:
  std::unique_ptr<a11y::A11yFocusManager> a11y_focus_manager_;
  MockA11yFocusManager* mock_a11y_focus_manager_ptr_;
  std::unique_ptr<a11y::Speaker> speaker_;
  MockSpeaker* mock_speaker_ptr_;
};

}  // namespace accessibility_test

#endif  // SRC_UI_A11Y_LIB_SCREEN_READER_TESTS_MOCKS_MOCK_SCREEN_READER_CONTEXT_H_
