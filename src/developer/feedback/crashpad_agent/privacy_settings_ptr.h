// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_FEEDBACK_CRASHPAD_AGENT_PRIVACY_SETTINGS_PTR_H_
#define SRC_DEVELOPER_FEEDBACK_CRASHPAD_AGENT_PRIVACY_SETTINGS_PTR_H_

#include <fuchsia/settings/cpp/fidl.h>
#include <lib/sys/cpp/service_directory.h>

#include <memory>

#include "src/lib/fxl/macros.h"
#include "third_party/crashpad/client/settings.h"

namespace feedback {

// Updates the "enable upload" field in Crashpad's database settings on "user data sharing consent"
// changes.
//
// In case of failure, e.g., loss of connection, error returned, "enable upload" is set to false
// regardless of its current state.
//
// Wraps around fuchsia::settings::PrivacyPtr to handle establishing the connection, losing the
// connection, waiting for the callback, etc.
class PrivacySettingsWatcher {
 public:
  // fuchsia.settings.Privacy is expected to be in |services|.
  PrivacySettingsWatcher(std::shared_ptr<sys::ServiceDirectory> services,
                         crashpad::Settings* crashpad_database_settings);

  // Connects to fuchsia.settings.Privacy and watches for "user data sharing consent" changes.
  void StartWatching();

  // Whether the watcher is currently connected to fuchsia.settings.Privacy.
  //
  // Mostly for testing purposes.
  bool IsConnected() { return privacy_settings_ptr_.is_bound(); };

  // Mostly for testing purposes.
  const fuchsia::settings::PrivacySettings& privacy_settings() { return privacy_settings_; }

 private:
  void Connect();
  void Watch();
  void Reset();
  void Update();

  const std::shared_ptr<sys::ServiceDirectory> services_;
  crashpad::Settings* crashpad_database_settings_;

  fuchsia::settings::PrivacySettings privacy_settings_;
  fuchsia::settings::PrivacyPtr privacy_settings_ptr_;

  FXL_DISALLOW_COPY_AND_ASSIGN(PrivacySettingsWatcher);
};

}  // namespace feedback

#endif  // SRC_DEVELOPER_FEEDBACK_CRASHPAD_AGENT_PRIVACY_SETTINGS_PTR_H_
