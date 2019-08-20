// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_A11Y_BIN_A11Y_MANAGER_APP_H_
#define SRC_UI_A11Y_BIN_A11Y_MANAGER_APP_H_

#include <fuchsia/accessibility/cpp/fidl.h>
#include <fuchsia/sys/cpp/fidl.h>
#include <lib/sys/cpp/component_context.h>

#include <memory>

#include "lib/fidl/cpp/binding_set.h"
#include "src/lib/fxl/logging.h"
#include "src/lib/fxl/macros.h"
#include "src/ui/a11y/lib/screen_reader/screen_reader.h"
#include "src/ui/a11y/lib/semantics/semantics_manager_impl.h"
#include "src/ui/a11y/lib/settings/settings_manager_impl.h"
#include "src/ui/a11y/lib/tts/log_engine.h"
#include "src/ui/a11y/lib/tts/tts_manager_impl.h"

namespace a11y_manager {

// A11y manager application entry point.
class App : public fuchsia::accessibility::SettingsWatcher {
 public:
  explicit App(std::unique_ptr<sys::ComponentContext> context);
  ~App() = default;

  // |fuchsia::accessibility::SettingsWatcher|
  void OnSettingsChange(fuchsia::accessibility::Settings provided_settings) override;

  // Returns copy of current set of settings owned by A11y Manager.
  fuchsia::accessibility::SettingsPtr GetSettings();

 private:
  // Initialize function for the App.
  void Initialize();

  // Helper function to copy given settings to member variable.
  void SetSettings(fuchsia::accessibility::Settings provided_settings);

  // Initializes Screen Reader pointer when screen reader is enabled, and destroys
  // the pointer when Screen Reader is disabled.
  void OnScreenReaderEnabled(bool enabled);

  std::unique_ptr<sys::ComponentContext> startup_context_;

  // Pointer to Settings Manager Implementation.
  std::unique_ptr<a11y::SettingsManagerImpl> settings_manager_;

  // Pointer to Semantics Manager Implementation.
  std::unique_ptr<a11y::SemanticsManagerImpl> semantics_manager_;

  // Pointer to Screen Reader.
  std::unique_ptr<a11y::ScreenReader> screen_reader_;

  // Pointer to TTS Manager.
  std::unique_ptr<a11y::TtsManager> tts_manager_;

  // A simple Tts engine which logs output.
  // this object is constructed when Init() is called.
  std::unique_ptr<a11y::LogEngine> log_engine_;

  fidl::BindingSet<fuchsia::accessibility::SettingsWatcher> settings_watcher_bindings_;

  // Private variable for storing A11y Settings.
  fuchsia::accessibility::Settings settings_;

  // Pointer to SettingsManager service, which will be used for connecting App
  // to settings manager as a Watcher.
  fuchsia::accessibility::SettingsManagerPtr settings_manager_ptr_;

  FXL_DISALLOW_COPY_AND_ASSIGN(App);
};

}  // namespace a11y_manager

#endif  // SRC_UI_A11Y_BIN_A11Y_MANAGER_APP_H_
