// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_A11Y_LIB_SETTINGS_SETTINGS_MANAGER_H_
#define SRC_UI_A11Y_LIB_SETTINGS_SETTINGS_MANAGER_H_

#include <fuchsia/accessibility/cpp/fidl.h>
#include <lib/fidl/cpp/binding_set.h>

#include <src/lib/fxl/macros.h>

#include "src/ui/a11y/lib/settings/settings_provider.h"

namespace a11y {
class SettingsManager : public fuchsia::accessibility::SettingsManager {
 public:
  SettingsManager();
  ~SettingsManager() override;

  void AddBinding(fidl::InterfaceRequest<fuchsia::accessibility::SettingsManager> request);

  // |fuchsia::accessibility::SettingsManager|:
  void RegisterSettingProvider(fidl::InterfaceRequest<fuchsia::accessibility::SettingsProvider>
                                   settings_provider_request) override;

  // |fuchsia::accessibility::SettingsManager|:
  void Watch(fidl::InterfaceHandle<fuchsia::accessibility::SettingsWatcher> watcher) override;

 private:
  fidl::BindingSet<fuchsia::accessibility::SettingsManager> bindings_;
  SettingsProvider settings_provider_;

  FXL_DISALLOW_COPY_AND_ASSIGN(SettingsManager);
};

}  // namespace a11y

#endif  // SRC_UI_A11Y_LIB_SETTINGS_SETTINGS_MANAGER_H_
