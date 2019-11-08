// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/a11y/lib/screen_reader/screen_reader.h"

#include "src/lib/syslog/cpp/logger.h"

namespace a11y {

ScreenReader::ScreenReader(a11y::SemanticsManager* semantics_manager, a11y::TtsManager* tts_manager,
                           a11y::GestureManager* gesture_manager)
    : tts_manager_(tts_manager), gesture_manager_(gesture_manager) {
  action_context_ = std::make_unique<ScreenReaderAction::ActionContext>();
  action_context_->semantics_manager = semantics_manager;

  InitializeServicesAndAction();
}

void ScreenReader::InitializeServicesAndAction() {
  // Initialize TTS Engine which will be used for Speaking using TTS.
  // TTS engine is stored in action_context_.
  tts_manager_->OpenEngine(action_context_->tts_engine_ptr.NewRequest(),
                           [](fuchsia::accessibility::tts::TtsManager_OpenEngine_Result result) {
                             if (result.is_err()) {
                               FX_LOGS(ERROR) << "Tts Manager failed to Open Engine.";
                             }
                           });

  // Initialize Screen reader supported "Actions".
  actions_.insert({"explore_action", std::make_unique<a11y::ExploreAction>(action_context_.get())});

  gesture_manager_->gesture_handler()->BindOneFingerTapAction(
      [this](zx_koid_t viewref_koid, fuchsia::math::PointF point) {
        ScreenReaderAction::ActionData action_data;
        action_data.koid = viewref_koid;
        action_data.local_point = point;
        ExecuteAction("explore_action", action_data);
      });
}

bool ScreenReader::ExecuteAction(std::string action_name,
                                 ScreenReaderAction::ActionData action_data) {
  auto action_pair = actions_.find(action_name);
  if (action_pair == actions_.end()) {
    FX_LOGS(ERROR) << "No action found with string :" << action_name;
    return false;
  }
  action_pair->second->Run(action_data);
  return true;
}

}  // namespace a11y
