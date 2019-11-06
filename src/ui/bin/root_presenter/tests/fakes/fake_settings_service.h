// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_BIN_ROOT_PRESENTER_TESTS_FAKES_FAKE_SETTINGS_SERVICE_H_
#define SRC_UI_BIN_ROOT_PRESENTER_TESTS_FAKES_FAKE_SETTINGS_SERVICE_H_

#include <fuchsia/accessibility/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/sys/cpp/component_context.h>
#include <math.h>

#include "src/lib/fxl/macros.h"

namespace root_presenter {
namespace testing {

// This is a mock of Fuchsia Settings Service, which provides all the
// Accessibility settings, and used solely for testing A11y Watcher in Root
// Presenter.
class FakeSettingsService {
 public:
  explicit FakeSettingsService(sys::ComponentContext& context);
  ~FakeSettingsService() = default;

  void SetMagnificationEnabled(
      bool magnification_enabled,
      fuchsia::accessibility::SettingsProvider::SetMagnificationEnabledCallback callback);

  void SetMagnificationZoomFactor(
      float magnification_zoom_factor,
      fuchsia::accessibility::SettingsProvider::SetMagnificationZoomFactorCallback callback);

  void SetScreenReaderEnabled(
      bool screen_reader_enabled,
      fuchsia::accessibility::SettingsProvider::SetScreenReaderEnabledCallback callback);

  void SetColorInversionEnabled(
      bool color_inversion_enabled,
      fuchsia::accessibility::SettingsProvider::SetColorInversionEnabledCallback callback);

  void SetColorCorrection(
      fuchsia::accessibility::ColorCorrection color_correction,
      fuchsia::accessibility::SettingsProvider::SetColorCorrectionCallback callback);

 private:
  fuchsia::accessibility::SettingsManagerPtr manager_;
  fuchsia::accessibility::SettingsProviderPtr settings_provider_ptr_;
  fuchsia::accessibility::Settings settings_;

  FXL_DISALLOW_COPY_AND_ASSIGN(FakeSettingsService);
};

}  // namespace testing
}  // namespace root_presenter

#endif  // SRC_UI_BIN_ROOT_PRESENTER_TESTS_FAKES_FAKE_SETTINGS_SERVICE_H_
