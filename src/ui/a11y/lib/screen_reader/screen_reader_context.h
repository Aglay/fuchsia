// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_A11Y_LIB_SCREEN_READER_SCREEN_READER_CONTEXT_H_
#define SRC_UI_A11Y_LIB_SCREEN_READER_SCREEN_READER_CONTEXT_H_

#include <lib/async/cpp/executor.h>

#include <memory>

#include "src/ui/a11y/lib/screen_reader/focus/a11y_focus_manager.h"

namespace a11y {

// ScreenReaderContext class stores the current state of the screen reader which includes the
// currently selected node(via the a11y focus manager) and state(currently selected semantic level).
// This class will be queried by "Actions" to get screen reader information.
// TODO(fxb/45886): Add support to store currently selected semantic level inside the
// ScreenReaderContext class.
class ScreenReaderContext {
 public:
  // Describes Screen Reader possible modes of navigation.
  enum class ScreenReaderMode {
    kNormal,  // Default case.
    // Whether a continuous exploration is in progress. A continuous exploration is a state
    // where an user is exploring the screen (by touch, for example), and is informed of the
    // elements in a11y focus (hearing the tts, for example). When in continuous exploration, if the
    // user stops at a particular semantic node, this node is informed only once, and another update
    // will only come after the user moves to a different node. In contrast, when the user is not in
    // continuous exploration, if the node is explored multiple times, they will always be informed.
    kContinuousExploration,
  };

  explicit ScreenReaderContext(std::unique_ptr<A11yFocusManager> a11y_focus_manager);

  virtual ~ScreenReaderContext() = default;

  // Returns pointer to A11yFocusManager which stores a11y focus information for screen reader.
  virtual A11yFocusManager* GetA11yFocusManager();

  // Returns the Executor used by the Screen Reader to schedule promises.
  async::Executor* executor() { return &executor_; }

  // Sets the Screen Reader current mode.
  void set_mode(ScreenReaderMode mode) { mode_ = mode; }
  ScreenReaderMode mode() const { return mode_; }

 private:
  async::Executor executor_;

  // Stores A11yFocusManager pointer.
  // A11yFocusManager pointer should never be nullptr.
  std::unique_ptr<A11yFocusManager> a11y_focus_manager_;

  // Current Screen Reader mode.
  ScreenReaderMode mode_ = ScreenReaderMode::kNormal;
};

}  // namespace a11y

#endif  // SRC_UI_A11Y_LIB_SCREEN_READER_SCREEN_READER_CONTEXT_H_
