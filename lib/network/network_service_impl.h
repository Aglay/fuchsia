// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_LIB_NETWORK_NETWORK_SERVICE_IMPL_H_
#define PERIDOT_LIB_NETWORK_NETWORK_SERVICE_IMPL_H_

#include "garnet/lib/backoff/backoff.h"
#include "garnet/lib/callback/auto_cleanable.h"
#include "garnet/lib/callback/scoped_task_runner.h"
#include "lib/fxl/tasks/task_runner.h"
#include "lib/network/fidl/network_service.fidl.h"
#include "peridot/lib/network/network_service.h"

namespace ledger {

class NetworkServiceImpl : public NetworkService {
 public:
  NetworkServiceImpl(
      fxl::RefPtr<fxl::TaskRunner> task_runner,
      std::unique_ptr<backoff::Backoff> backoff,
      std::function<network::NetworkServicePtr()> network_service_factory);
  ~NetworkServiceImpl() override;

  fxl::RefPtr<callback::Cancellable> Request(
      std::function<network::URLRequestPtr()> request_factory,
      std::function<void(network::URLResponsePtr)> callback) override;

 private:
  class RunningRequest;

  network::NetworkService* GetNetworkService();

  void RetryGetNetworkService();

  std::unique_ptr<backoff::Backoff> backoff_;
  bool in_backoff_ = false;
  std::function<network::NetworkServicePtr()> network_service_factory_;
  network::NetworkServicePtr network_service_;
  callback::AutoCleanableSet<RunningRequest> running_requests_;

  // Must be the last member field.
  callback::ScopedTaskRunner task_runner_;
};

}  // namespace ledger

#endif  // PERIDOT_LIB_NETWORK_NETWORK_SERVICE_IMPL_H_
