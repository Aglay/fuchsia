// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "low_energy_discovery_manager.h"

#include "apps/bluetooth/lib/hci/legacy_low_energy_scanner.h"
#include "apps/bluetooth/lib/hci/transport.h"
#include "lib/ftl/functional/make_copyable.h"
#include "lib/ftl/logging.h"

namespace bluetooth {
namespace gap {

LowEnergyDiscoverySession::LowEnergyDiscoverySession(
    ftl::WeakPtr<LowEnergyDiscoveryManager> manager)
    : active_(true), manager_(manager) {
  FTL_DCHECK(manager_);
  // Configured by default for the GAP General Discovery procedure.
  filter_.set_flags(static_cast<uint8_t>(AdvFlag::kLELimitedDiscoverableMode) |
                    static_cast<uint8_t>(AdvFlag::kLEGeneralDiscoverableMode));
}

LowEnergyDiscoverySession::~LowEnergyDiscoverySession() {
  FTL_DCHECK(thread_checker_.IsCreationThreadCurrent());
  if (active_) Stop();
}

void LowEnergyDiscoverySession::SetResultCallback(const DeviceFoundCallback& callback) {
  device_found_callback_ = callback;
  if (!manager_) return;
  for (const auto& cached_result : manager_->cached_scan_results()) {
    NotifyDiscoveryResult(cached_result.second.result, cached_result.second.data);
  }
}

void LowEnergyDiscoverySession::Stop() {
  FTL_DCHECK(thread_checker_.IsCreationThreadCurrent());
  FTL_DCHECK(active_);
  if (manager_) manager_->RemoveSession(this);
  active_ = false;
}

void LowEnergyDiscoverySession::NotifyDiscoveryResult(const hci::LowEnergyScanResult& result,
                                                      const common::ByteBuffer& data) const {
  if (device_found_callback_ && filter_.MatchLowEnergyResult(result, data)) {
    device_found_callback_(result, data);
  }
}

// TODO(armansito): data.CopyContents() dynamically allocates memory for and copies the contents of
// |data|. The memory management needs optimization. Improve this when adding a generalized
// DeviceCache class.
LowEnergyDiscoveryManager::CachedScanResult::CachedScanResult(
    const hci::LowEnergyScanResult& result, const common::ByteBuffer& data)
    : result(result), data(data.GetSize(), data.CopyContents()) {}

LowEnergyDiscoveryManager::CachedScanResult&
LowEnergyDiscoveryManager::CachedScanResult::CachedScanResult::operator=(CachedScanResult&& other) {
  result = other.result;
  data = std::move(other.data);
  return *this;
}

LowEnergyDiscoveryManager::LowEnergyDiscoveryManager(Mode mode, ftl::RefPtr<hci::Transport> hci,
                                                     ftl::RefPtr<ftl::TaskRunner> task_runner)
    : task_runner_(task_runner), weak_ptr_factory_(this) {
  FTL_DCHECK(hci);
  FTL_DCHECK(task_runner_);
  FTL_DCHECK(task_runner_->RunsTasksOnCurrentThread());

  // We currently do not support the Extended Advertising feature.
  FTL_DCHECK(mode == Mode::kLegacy);

  scanner_ = std::make_unique<hci::LegacyLowEnergyScanner>(this, hci, task_runner);
}

LowEnergyDiscoveryManager::~LowEnergyDiscoveryManager() {
  // TODO(armansito): Invalidate all known session objects here.
}

void LowEnergyDiscoveryManager::StartDiscovery(const SessionCallback& callback) {
  FTL_DCHECK(task_runner_->RunsTasksOnCurrentThread());
  FTL_DCHECK(callback);

  // If a request to start or stop is currently pending then this one will become pending until the
  // HCI request completes (this does NOT include the state in which we are stopping and restarting
  // scan in between scan periods).
  if (!pending_.empty() ||
      (scanner_->state() == hci::LowEnergyScanner::State::kStopping && sessions_.empty())) {
    FTL_DCHECK(!scanner_->IsScanning());
    pending_.push(callback);
    return;
  }

  // If a device scan is already in progress, then the request succeeds (this includes the state in
  // which we are stopping and restarting scan in between scan periods).
  if (!sessions_.empty()) {
    FTL_DCHECK(scanner_->IsScanning());

    // Invoke |callback| asynchronously.
    auto session = AddSession();
    task_runner_->PostTask(ftl::MakeCopyable(
        [ callback, session = std::move(session) ]() mutable { callback(std::move(session)); }));
    return;
  }

  FTL_DCHECK(scanner_->state() == hci::LowEnergyScanner::State::kIdle);

  pending_.push(callback);
  StartScan();
}

std::unique_ptr<LowEnergyDiscoverySession> LowEnergyDiscoveryManager::AddSession() {
  // Cannot use make_unique here since LowEnergyDiscoverySession has a private constructor.
  std::unique_ptr<LowEnergyDiscoverySession> session(
      new LowEnergyDiscoverySession(weak_ptr_factory_.GetWeakPtr()));
  FTL_DCHECK(sessions_.find(session.get()) == sessions_.end());
  sessions_.insert(session.get());
  return session;
}

void LowEnergyDiscoveryManager::RemoveSession(LowEnergyDiscoverySession* session) {
  FTL_DCHECK(task_runner_->RunsTasksOnCurrentThread());
  FTL_DCHECK(session);

  // Only active sessions are allowed to call this method. If there is at least one active session
  // object out there, then we MUST be scanning.
  FTL_DCHECK(session->active());

  FTL_DCHECK(sessions_.find(session) != sessions_.end());
  sessions_.erase(session);

  // Stop scanning if the session count has dropped to zero.
  if (sessions_.empty()) scanner_->StopScan();
}

void LowEnergyDiscoveryManager::OnDeviceFound(const hci::LowEnergyScanResult& result,
                                              const common::ByteBuffer& data) {
  FTL_DCHECK(task_runner_->RunsTasksOnCurrentThread());

  // TODO(armansito): The CachedScanResult constructor dynamically allocates memory for and copies
  // the contents of |data|. The memory management needs optimization.
  cached_scan_results_[result.address] = CachedScanResult(result, data);

  for (const auto& session : sessions_) {
    session->NotifyDiscoveryResult(result, data);
  }
}

void LowEnergyDiscoveryManager::OnScanStatus(hci::LowEnergyScanner::Status status) {
  switch (status) {
    case hci::LowEnergyScanner::Status::kFailed:
      FTL_LOG(ERROR) << "gap: LowEnergyDiscoveryManager: Failed to start discovery!";
      FTL_DCHECK(sessions_.empty());

      // Report failure on all currently pending requests. If any of the callbacks issue a retry
      // the new requests will get re-queued and notified of failure in the same loop here.
      while (!pending_.empty()) {
        auto& callback = pending_.front();
        callback(nullptr);

        pending_.pop();
      }
      break;
    case hci::LowEnergyScanner::Status::kStarted:
      FTL_LOG(INFO) << "gap: LowEnergyDiscoveryManager: Started scanning";

      // Create and register all sessions before notifying the clients. We do this so that the
      // reference count is incremented for all new sessions before the callbacks execute, to
      // prevent a potential case in which a callback stops its session immediately which could
      // cause the reference count to drop the zero before all clients receive their session object.
      if (!pending_.empty()) {
        size_t count = pending_.size();
        std::unique_ptr<LowEnergyDiscoverySession> new_sessions[count];
        std::generate(new_sessions, new_sessions + count, [this] { return AddSession(); });
        for (size_t i = 0; i < count; i++) {
          auto& callback = pending_.front();
          callback(std::move(new_sessions[i]));

          pending_.pop();
        }
      }
      FTL_DCHECK(pending_.empty());
      break;
    case hci::LowEnergyScanner::Status::kStopped:
      // TODO(armansito): Revise this logic when we support pausing a scan even with active
      // sessions.
      FTL_LOG(INFO) << "gap: LowEnergyDiscoveryManager: Stopped scanning";

      cached_scan_results_.clear();

      // Some clients might have requested to start scanning while we were waiting for it to stop.
      // Restart scanning if that is the case.
      if (!pending_.empty()) StartScan();
      break;
    case hci::LowEnergyScanner::Status::kComplete:
      FTL_LOG(INFO) << "gap: LowEnergyDiscoveryManager: Continuing periodic scan";
      FTL_DCHECK(!sessions_.empty());
      FTL_DCHECK(pending_.empty());

      cached_scan_results_.clear();

      // The scan period has completed. Restart scanning.
      StartScan();
      break;
  }
}

void LowEnergyDiscoveryManager::StartScan() {
  auto cb = [self = weak_ptr_factory_.GetWeakPtr()](auto status) {
    if (self) self->OnScanStatus(status);
  };

  // TODO(armansito): For now we always do an active scan. When we support the auto-connection
  // procedure we should also implement background scanning using the controller white list.
  // TODO(armansito): Use the appropriate "slow" interval & window values for background scanning.
  // TODO(armansito): A client that is interested in scanning nearby beacons and calculating
  // proximity based on RSSI changes may want to disable duplicate filtering. We generally shouldn't
  // allow this unless a client has the capability for it. Processing all HCI events containing
  // advertising reports will both generate a lot of bus traffic and performing duplicate filtering
  // on the host will take away CPU cycles from other things. It's a valid use case but needs proper
  // management. For now we always make the controller filter duplicate reports.

  // Since we use duplicate filtering, we stop and start the scan periodically to re-process
  // advertisements. We use the minimum required scan period for general discovery (by default;
  // |scan_period_| can be modified, e.g. by unit tests).
  scanner_->StartScan(true /* active */, kLEScanFastInterval, kLEScanFastWindow,
                      true /* filter_duplicates */, hci::LEScanFilterPolicy::kNoWhiteList,
                      scan_period_, cb);
}

}  // namespace gap
}  // namespace bluetooth
