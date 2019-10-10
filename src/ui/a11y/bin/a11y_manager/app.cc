// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/a11y/bin/a11y_manager/app.h"

#include <zircon/status.h>

#include "src/lib/syslog/cpp/logger.h"

namespace a11y_manager {

const float kDefaultMagnificationZoomFactor = 1.0;

App::App(std::unique_ptr<sys::ComponentContext> context)
    : startup_context_(std::move(context)),
      // The following services publish themselves upon initialization.
      semantics_manager_(startup_context_.get()),
      tts_manager_(startup_context_.get()),
      // For now, we use a simple Tts Engine which only logs the output.
      // On initialization, it registers itself with the Tts manager.
      log_engine_(startup_context_.get()) {
  startup_context_->outgoing()->AddPublicService(
      settings_manager_bindings_.GetHandler(&settings_manager_));

  // Register a11y manager as a settings provider.
  settings_manager_.RegisterSettingProvider(settings_provider_ptr_.NewRequest());
  settings_provider_ptr_.set_error_handler([](zx_status_t status) {
    FX_LOGS(ERROR) << "Error from fuchsia::accessibility::settings::SettingsProvider"
                   << zx_status_get_string(status);
  });

  // Connect to Root presenter service.
  pointer_event_registry_ =
      startup_context_->svc()->Connect<fuchsia::ui::input::accessibility::PointerEventRegistry>();
  pointer_event_registry_.set_error_handler([](zx_status_t status) {
    FX_LOGS(ERROR) << "Error from fuchsia::ui::input::accessibility::PointerEventRegistry"
                   << zx_status_get_string(status);
  });

  // Connect to setui.
  setui_settings_ = startup_context_->svc()->Connect<fuchsia::settings::Accessibility>();
  setui_settings_.set_error_handler([](zx_status_t status) {
    FX_LOGS(ERROR) << "Error from fuchsia::settings::Accessibility" << zx_status_get_string(status);
  });

  // Start watching setui for current settings
  WatchSetui();
}

App::~App() = default;

void InternalSettingsCallback(fuchsia::accessibility::SettingsManagerStatus status) {
  if (status == fuchsia::accessibility::SettingsManagerStatus::ERROR) {
    FX_LOGS(ERROR) << "Error writing internal accessibility settings.";
  }
}

// This currently ignores errors in the internal settings API. That API is being removed in favor of
// smaller feature-oriented APIs.
void App::UpdateInternalSettings(const fuchsia::settings::AccessibilitySettings& systemSettings) {
  if (systemSettings.has_screen_reader()) {
    settings_provider_ptr_->SetScreenReaderEnabled(systemSettings.screen_reader(),
                                                   InternalSettingsCallback);
    ToggleScreenReaderSetting(systemSettings.screen_reader());
  }
  if (systemSettings.has_color_inversion()) {
    settings_provider_ptr_->SetColorInversionEnabled(systemSettings.color_inversion(),
                                                     InternalSettingsCallback);
  }
  if (systemSettings.has_enable_magnification()) {
    settings_provider_ptr_->SetMagnificationEnabled(systemSettings.enable_magnification(),
                                                    InternalSettingsCallback);
  }
  if (systemSettings.has_color_correction()) {
    switch (systemSettings.color_correction()) {
      case fuchsia::settings::ColorBlindnessType::NONE:
        settings_provider_ptr_->SetColorCorrection(
            fuchsia::accessibility::ColorCorrection::DISABLED, InternalSettingsCallback);
        break;
      case fuchsia::settings::ColorBlindnessType::PROTANOMALY:
        settings_provider_ptr_->SetColorCorrection(
            fuchsia::accessibility::ColorCorrection::CORRECT_PROTANOMALY, InternalSettingsCallback);
        break;
      case fuchsia::settings::ColorBlindnessType::DEUTERANOMALY:
        settings_provider_ptr_->SetColorCorrection(
            fuchsia::accessibility::ColorCorrection::CORRECT_DEUTERANOMALY,
            InternalSettingsCallback);
        break;
      case fuchsia::settings::ColorBlindnessType::TRITANOMALY:
        settings_provider_ptr_->SetColorCorrection(
            fuchsia::accessibility::ColorCorrection::CORRECT_TRITANOMALY, InternalSettingsCallback);
        break;
    }
  }
}

void App::SetuiWatchCallback(fuchsia::settings::Accessibility_Watch_Result result) {
  if (result.is_err()) {
    FX_LOGS(ERROR) << "Error reading setui accessibility settings.";
  } else if (result.is_response()) {
    UpdateInternalSettings(result.response().settings);
  }
  WatchSetui();
}

void App::WatchSetui() { setui_settings_->Watch(fit::bind_member(this, &App::SetuiWatchCallback)); }

fuchsia::accessibility::SettingsPtr App::GetSettings() { return settings_manager_.GetSettings(); }
void App::OnScreenReaderEnabled(bool enabled) {
  // Reset SemanticsTree and registered views in SemanticsManagerImpl.
  semantics_manager_.SetSemanticsManagerEnabled(enabled);

  // Reset ScreenReader.
  if (enabled) {
    screen_reader_ = std::make_unique<a11y::ScreenReader>(&semantics_manager_, &tts_manager_,
                                                          gesture_manager_.get());
  } else {
    screen_reader_.reset();
  }
}

void App::OnAccessibilityPointerEventListenerEnabled(bool enabled) {
  if (enabled) {
    gesture_manager_ = std::make_unique<a11y::GestureManager>();
    pointer_event_registry_->Register(gesture_manager_->binding().NewBinding());
  } else {
    gesture_manager_.reset();
  }
}

void App::ToggleScreenReaderSetting(bool new_screen_reader_enabled_value) {
  fuchsia::accessibility::SettingsPtr settings_ptr = settings_manager_.GetSettings();
  const bool old_screen_reader_enabled_value =
      settings_ptr->has_screen_reader_enabled() && settings_ptr->screen_reader_enabled();
  if (new_screen_reader_enabled_value != old_screen_reader_enabled_value) {
    OnAccessibilityPointerEventListenerEnabled(new_screen_reader_enabled_value);
    OnScreenReaderEnabled(new_screen_reader_enabled_value);
  }
}

}  // namespace a11y_manager
