// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/feedback/reboot_info/reboot_reason.h"

#include "src/lib/syslog/cpp/logger.h"

namespace feedback {
namespace {

std::string ToString(const RebootReason reboot_reason) {
  switch (reboot_reason) {
    case RebootReason::kNotSet:
      return "RebootReason::kNotSet";
    case RebootReason::kNotParseable:
      return "RebootReason::kNotParseable";
    case RebootReason::kGenericGraceful:
      return "RebootReason::kGenericGraceful";
    case RebootReason::kCold:
      return "RebootReason::kCold";
    case RebootReason::kSpontaneous:
      return "RebootReason::kSpontaneous";
    case RebootReason::kKernelPanic:
      return "RebootReason::kKernelPanic";
    case RebootReason::kOOM:
      return "RebootReason::kOOM";
    case RebootReason::kHardwareWatchdogTimeout:
      return "RebootReason::kHardwareWatchdogTimeout";
    case RebootReason::kSoftwareWatchdogTimeout:
      return "RebootReason::kSoftwareWatchdogTimeout";
    case RebootReason::kBrownout:
      return "RebootReason::kBrownout";
  }
}

}  // namespace

bool IsGraceful(const RebootReason reboot_reason) {
  return reboot_reason == RebootReason::kGenericGraceful;
}

cobalt::RebootReason ToCobaltRebootReason(const RebootReason reboot_reason) {
  switch (reboot_reason) {
    case RebootReason::kNotParseable:
      // TODO(50946): Stop assuming a kernel panic if the file can't be parsed.
      return cobalt::RebootReason::kKernelPanic;
    case RebootReason::kGenericGraceful:
      return cobalt::RebootReason::kClean;
    case RebootReason::kCold:
      return cobalt::RebootReason::kCold;
    case RebootReason::kSpontaneous:
      return cobalt::RebootReason::kUnknown;
    case RebootReason::kKernelPanic:
      return cobalt::RebootReason::kKernelPanic;
    case RebootReason::kOOM:
      return cobalt::RebootReason::kOOM;
    case RebootReason::kHardwareWatchdogTimeout:
      return cobalt::RebootReason::kHardwareWatchdog;
    case RebootReason::kSoftwareWatchdogTimeout:
      return cobalt::RebootReason::kSoftwareWatchdog;
    case RebootReason::kBrownout:
      return cobalt::RebootReason::kBrownout;
    case RebootReason::kNotSet:
      FX_LOGS(FATAL) << "Not expecting a Cobalt reboot reason for " << ToString(reboot_reason);
      return cobalt::RebootReason::kKernelPanic;
  }
}

std::string ToCrashSignature(const RebootReason reboot_reason) {
  switch (reboot_reason) {
    case RebootReason::kNotParseable:
      // TODO(50946): Stop assuming a kernel panic if the file can't be parsed.
      return "fuchsia-kernel-panic";
    case RebootReason::kSpontaneous:
      // TODO(50946): Change this to a better crash signature, most likely "brief-power-loss".
      return "fuchsia-reboot-unknown";
    case RebootReason::kKernelPanic:
      return "fuchsia-kernel-panic";
    case RebootReason::kOOM:
      return "fuchsia-oom";
    case RebootReason::kHardwareWatchdogTimeout:
      return "fuchsia-hw-watchdog-timeout";
    case RebootReason::kSoftwareWatchdogTimeout:
      return "fuchsia-sw-watchdog-timeout";
    case RebootReason::kBrownout:
      return "fuchsia-brownout";
    case RebootReason::kNotSet:
    case RebootReason::kGenericGraceful:
    case RebootReason::kCold:
      FX_LOGS(FATAL) << "Not expecting a crash for reboot reason " << ToString(reboot_reason);
      return "FATAL ERROR";
  }
}

std::string ToCrashProgramName(const RebootReason reboot_reason) {
  switch (reboot_reason) {
    case RebootReason::kNotParseable:
    case RebootReason::kKernelPanic:
      // TODO(50946): Stop assuming a kernel panic if the file can't be parsed.
      return "kernel";
    case RebootReason::kBrownout:
    case RebootReason::kHardwareWatchdogTimeout:
    case RebootReason::kSpontaneous:
      return "device";
    case RebootReason::kOOM:
    case RebootReason::kSoftwareWatchdogTimeout:
      return "system";
    case RebootReason::kNotSet:
    case RebootReason::kGenericGraceful:
    case RebootReason::kCold:
      FX_LOGS(FATAL) << "Not expecting a program name request for reboot reason "
                     << ToString(reboot_reason);
      return "FATAL ERROR";
  }
}

std::optional<fuchsia::feedback::RebootReason> ToFidlRebootReason(
    const RebootReason reboot_reason) {
  switch (reboot_reason) {
    case RebootReason::kGenericGraceful:
      return fuchsia::feedback::RebootReason::GENERIC_GRACEFUL;
    case RebootReason::kCold:
      return fuchsia::feedback::RebootReason::COLD;
    case RebootReason::kSpontaneous:
      return fuchsia::feedback::RebootReason::BRIEF_POWER_LOSS;
    case RebootReason::kKernelPanic:
      return fuchsia::feedback::RebootReason::KERNEL_PANIC;
    case RebootReason::kOOM:
      return fuchsia::feedback::RebootReason::SYSTEM_OUT_OF_MEMORY;
    case RebootReason::kHardwareWatchdogTimeout:
      return fuchsia::feedback::RebootReason::HARDWARE_WATCHDOG_TIMEOUT;
    case RebootReason::kSoftwareWatchdogTimeout:
      return fuchsia::feedback::RebootReason::SOFTWARE_WATCHDOG_TIMEOUT;
    case RebootReason::kBrownout:
      return fuchsia::feedback::RebootReason::BROWNOUT;
    case RebootReason::kNotSet:
      FX_LOGS(FATAL) << "Not expecting a Feedback reboot reason for " << ToString(reboot_reason);
    case RebootReason::kNotParseable:
      return std::nullopt;
  }
}

}  // namespace feedback
