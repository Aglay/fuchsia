// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_A11Y_BIN_A11Y_MANAGER_APP_H_
#define SRC_UI_A11Y_BIN_A11Y_MANAGER_APP_H_

#include <fuchsia/accessibility/cpp/fidl.h>
#include <fuchsia/settings/cpp/fidl.h>
#include <fuchsia/sys/cpp/fidl.h>
#include <fuchsia/ui/input/accessibility/cpp/fidl.h>
#include <lib/fidl/cpp/binding.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/sys/cpp/component_context.h>

#include <memory>

#include "src/lib/fxl/macros.h"
#include "src/ui/a11y/lib/gesture_manager/gesture_manager.h"
#include "src/ui/a11y/lib/screen_reader/screen_reader.h"
#include "src/ui/a11y/lib/semantics/semantics_manager.h"
#include "src/ui/a11y/lib/settings/settings_manager.h"
#include "src/ui/a11y/lib/tts/log_engine.h"
#include "src/ui/a11y/lib/tts/tts_manager.h"

namespace a11y_manager {

// A11y manager application entry point.
class App : public fuchsia::accessibility::SettingsWatcher {
 public:
  explicit App(std::unique_ptr<sys::ComponentContext> context);
  ~App() override;

  // |fuchsia::accessibility::SettingsWatcher|
  void OnSettingsChange(fuchsia::accessibility::Settings provided_settings) override;

  // Returns a copy of current set of settings owned by A11y Manager.
  fuchsia::accessibility::SettingsPtr GetSettings();

 private:
  // Helper function to copy given settings to member variable.
  void SetSettings(fuchsia::accessibility::Settings provided_settings);

  // Callback for Setui's Watch() method.
  void SetuiWatchCallback(fuchsia::settings::Accessibility_Watch_Result result);

  // Set up continuous watch of setui's accessibility settings. The Watch(...) method returns on the
  // initial call, and afterwards uses a hanging get to return only when settings change.
  void WatchSetui();

  // Initializes Screen Reader pointer when screen reader is enabled, and destroys
  // the pointer when Screen Reader is disabled.
  void OnScreenReaderEnabled(bool enabled);

  // When enabled, starts the Accessibility Pointer Event Listener and
  // instantiates |gesture_manager_|. When disabled, it disconnects the Listener
  // and res resets |gesture_manager_|.
  void OnAccessibilityPointerEventListenerEnabled(bool enabled);

  // Makes changes to internal settings based on new settings from SetUI. This is not particularly
  // efficient since the existing internal API forces a new call to watchers for each changed
  // setting.
  // TODO(17180): This should be removed when the internal settings API is replaced with smaller
  // configuration APIs.
  void UpdateInternalSettings(const fuchsia::settings::AccessibilitySettings& systemSettings);

  std::unique_ptr<sys::ComponentContext> startup_context_;

  std::unique_ptr<a11y::ScreenReader> screen_reader_;
  a11y::SemanticsManager semantics_manager_;
  a11y::SettingsManager settings_manager_;
  a11y::TtsManager tts_manager_;
  // A simple Tts engine which logs output.
  a11y::LogEngine log_engine_;
  // The gesture manager is instantiated whenever a11y manager starts listening
  // for pointer events, and destroyed when the listener disconnects.
  std::unique_ptr<a11y::GestureManager> gesture_manager_;

  // TODO(17180): This will be removed and replaced this with smaller configuration APIs.
  fidl::BindingSet<fuchsia::accessibility::SettingsManager> settings_manager_bindings_;
  fidl::Binding<fuchsia::accessibility::SettingsWatcher> settings_watcher_binding_;
  fuchsia::accessibility::SettingsProviderPtr settings_provider_ptr_;

  fidl::BindingSet<fuchsia::ui::input::accessibility::PointerEventListener> listener_bindings_;

  fuchsia::accessibility::Settings settings_;

  // Interface between a11y manager and Root presenter to register a
  // accessibility pointer event listener.
  fuchsia::ui::input::accessibility::PointerEventRegistryPtr pointer_event_registry_;

  // Interface between Setui and a11y manager to get updates when user settings change.
  fuchsia::settings::AccessibilityPtr setui_settings_;

  FXL_DISALLOW_COPY_AND_ASSIGN(App);
};

}  // namespace a11y_manager

#endif  // SRC_UI_A11Y_BIN_A11Y_MANAGER_APP_H_
