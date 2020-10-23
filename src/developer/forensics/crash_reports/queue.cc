// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/forensics/crash_reports/queue.h"

#include <lib/async/cpp/task.h>
#include <lib/fit/defer.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/zx/time.h>
#include <zircon/errors.h>

#include "src/developer/forensics/crash_reports/constants.h"
#include "src/developer/forensics/crash_reports/info/queue_info.h"
#include "src/lib/fxl/strings/string_printf.h"

namespace forensics {
namespace crash_reports {

using async::PostDelayedTask;
using async::PostTask;
using crashpad::FileReader;
using UploadPolicy = Settings::UploadPolicy;

constexpr char kStorePath[] = "/tmp/reports";

void Queue::WatchSettings(Settings* settings) {
  settings->RegisterUploadPolicyWatcher(
      [this](const UploadPolicy& upload_policy) { OnUploadPolicyChange(upload_policy); });
}

Queue::Queue(async_dispatcher_t* dispatcher, std::shared_ptr<sys::ServiceDirectory> services,
             std::shared_ptr<InfoContext> info_context, CrashServer* crash_server)
    : dispatcher_(dispatcher),
      services_(services),
      store_(info_context, kStorePath, kStoreMaxSize),
      crash_server_(crash_server),
      info_(std::move(info_context)),
      network_reconnection_backoff_(/*initial_delay=*/zx::min(1), /*retry_factor=*/2u,
                                    /*max_delay=*/zx::hour(1)) {
  FX_CHECK(dispatcher_);

  ProcessAllEveryFifteenMinutes();
  ProcessAllOnNetworkReachable();

  // TODO(fxbug.dev/56448): Initialize queue with the reports in the store. We need to be able to
  // distinguish archived reports from reports that have not been uploaded yet.
}

bool Queue::Contains(const ReportId report_id) const {
  return std::find(pending_reports_.begin(), pending_reports_.end(), report_id) !=
         pending_reports_.end();
}

bool Queue::Add(Report report) {
  // Attempt to upload a report before putting it in the store.
  std::string server_report_id;
  if (state_ == State::Upload) {
    info_.RecordUploadAttemptNumber(1u);
    if (Upload(report, &server_report_id)) {
      info_.MarkReportAsUploaded(server_report_id, 1u);
      return true;
    }
  }

  std::vector<ReportId> garbage_collected_reports;
  std::optional<ReportId> local_report_id =
      store_.Add(std::move(report), &garbage_collected_reports);

  for (const auto& id : garbage_collected_reports) {
    GarbageCollect(id);
  }

  if (!local_report_id.has_value()) {
    return false;
  }

  pending_reports_.push_back(local_report_id.value());

  // Early upload that failed.
  if (state_ == State::Upload) {
    upload_attempts_[local_report_id.value()]++;
  } else if (state_ == State::Archive) {
    ArchiveAll();
  }

  return true;
}

size_t Queue::ProcessAll() {
  switch (state_) {
    case State::Archive:
      return ArchiveAll();
    case State::Upload:
      return UploadAll();
    case State::LeaveAsPending:
      return 0;
  }
}

bool Queue::Upload(const ReportId local_report_id) {
  std::optional<Report> report = store_.Get(local_report_id);
  if (!report.has_value()) {
    // |pending_reports_| is kept in sync with |store_| so Get should only ever fail if the report
    // is deleted from the store by an external influence, e.g., the filesystem flushes /cache.
    return true;
  }

  upload_attempts_[local_report_id]++;
  info_.RecordUploadAttemptNumber(upload_attempts_[local_report_id]);

  std::string server_report_id;
  if (Upload(report.value(), &server_report_id)) {
    info_.MarkReportAsUploaded(server_report_id, upload_attempts_[local_report_id]);
    upload_attempts_.erase(local_report_id);
    store_.Remove(local_report_id);
    return true;
  }

  FX_LOGS(WARNING) << "Error uploading local report " << std::to_string(local_report_id);

  return false;
}

bool Queue::Upload(const Report& report, std::string* server_report_id) {
  if (crash_server_->MakeRequest(report, server_report_id)) {
    FX_LOGS(INFO) << "Successfully uploaded report at https://crash.corp.google.com/"
                  << *server_report_id;
    return true;
  }

  return false;
}

void Queue::GarbageCollect(const ReportId local_report_id) {
  FX_LOGS(INFO) << "Garbage collected local report " << std::to_string(local_report_id);
  info_.MarkReportAsGarbageCollected(upload_attempts_[local_report_id]);
  upload_attempts_.erase(local_report_id);
  pending_reports_.erase(
      std::remove(pending_reports_.begin(), pending_reports_.end(), local_report_id),
      pending_reports_.end());
}

size_t Queue::UploadAll() {
  std::vector<ReportId> new_pending_reports;
  for (const auto& local_report_id : pending_reports_) {
    if (!Upload(local_report_id)) {
      new_pending_reports.push_back(local_report_id);
    }
  }

  pending_reports_.swap(new_pending_reports);

  // |new_pending_reports| now contains the pending reports before attempting to upload them.
  return new_pending_reports.size() - pending_reports_.size();
}

size_t Queue::ArchiveAll() {
  size_t successful = 0;
  for (const auto& local_report_id : pending_reports_) {
    FX_LOGS(INFO) << "Archiving local report " << std::to_string(local_report_id)
                  << " under /tmp/reports";
    info_.MarkReportAsArchived(upload_attempts_[local_report_id]);
  }

  pending_reports_.clear();

  return successful;
}

// The queue is inheritly conservative with uploading crash reports meaning that a report that is
// forbidden from being uploaded will never be uploaded while crash reports that are permitted to
// be uploaded may later be considered to be forbidden. This is due to the fact that when uploads
// are disabled all reports are immediately archived after having been added to the queue, thus we
// never have to worry that a report that shouldn't be uploaded ends up being uploaded when the
// upload policy changes.
void Queue::OnUploadPolicyChange(const Settings::UploadPolicy& upload_policy) {
  switch (upload_policy) {
    case UploadPolicy::DISABLED:
      state_ = State::Archive;
      break;
    case UploadPolicy::ENABLED:
      state_ = State::Upload;
      break;
    case UploadPolicy::LIMBO:
      state_ = State::LeaveAsPending;
      break;
  }
  ProcessAll();
}

void Queue::ProcessAllEveryFifteenMinutes() {
  if (const auto status = PostDelayedTask(
          dispatcher_,
          [this] {
            // Save the size of |pending_reports_| because ProcessAll mutates |pending_reports_|.
            if (const auto pending = pending_reports_.size(); pending > 0) {
              const auto processed = ProcessAll();
              FX_LOGS(INFO) << fxl::StringPrintf(
                  "Successfully processed %zu of %zu pending crash reports as part of the "
                  "15-minute periodic processing",
                  processed, pending);
            }
            ProcessAllEveryFifteenMinutes();
          },
          zx::min(15));
      status != ZX_OK) {
    FX_PLOGS(ERROR, status) << "Error posting periodic process task to async loop. Won't retry.";
  }
}

void Queue::ProcessAllOnNetworkReachable() {
  netstack_ = services_->Connect<fuchsia::netstack::Netstack>();
  netstack_.set_error_handler([this](zx_status_t status) {
    FX_PLOGS(ERROR, status) << "Lost connection to " << fuchsia::netstack::Netstack::Name_;

    network_reconnection_task_.Reset([this]() mutable { ProcessAllOnNetworkReachable(); });
    async::PostDelayedTask(
        dispatcher_, [cb = network_reconnection_task_.callback()]() { cb(); },
        network_reconnection_backoff_.GetNext());
  });

  auto isReachable = [](const fuchsia::netstack::NetInterface& interface) {
    if ((interface.flags & fuchsia::netstack::Flags::UP) != fuchsia::netstack::Flags::UP) {
      return false;
    }
    if ((interface.flags & fuchsia::netstack::Flags::DHCP) != fuchsia::netstack::Flags::DHCP) {
      return false;
    }
    auto isZero = [](const uint8_t octet) { return octet == 0; };
    switch (interface.addr.Which()) {
      case fuchsia::net::IpAddress::Tag::kIpv4: {
        const auto& octets = interface.addr.ipv4().addr;
        return !std::all_of(octets.cbegin(), octets.cend(), isZero);
      }
      case fuchsia::net::IpAddress::Tag::kIpv6: {
        const auto& octets = interface.addr.ipv6().addr;
        return !std::all_of(octets.cbegin(), octets.cend(), isZero);
      }
      case fuchsia::net::IpAddress::Tag::Invalid: {
        FX_LOGS(ERROR) << "Network interface " << interface.name << " has malformed IP address";
        return false;
      }
    }
  };

  netstack_.events().OnInterfacesChanged =
      [this, isReachable](std::vector<fuchsia::netstack::NetInterface> interfaces) {
        network_reconnection_backoff_.Reset();
        const bool reachable = std::any_of(interfaces.cbegin(), interfaces.cend(), isReachable);
        if (reachable) {
          // Save the size of |pending_reports_| because ProcessAll mutates |pending_reports_|.
          if (const auto pending = pending_reports_.size(); pending > 0) {
            const auto processed = ProcessAll();
            FX_LOGS(INFO) << fxl::StringPrintf(
                "Successfully processed %zu of %zu pending crash reports on network reachable",
                processed, pending);
          }
        }
      };
}

}  // namespace crash_reports
}  // namespace forensics
