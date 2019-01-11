// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_LEDGER_TESTING_NETCONNECTOR_FAKE_NETCONNECTOR_H_
#define PERIDOT_BIN_LEDGER_TESTING_NETCONNECTOR_FAKE_NETCONNECTOR_H_

#include <fuchsia/netconnector/cpp/fidl.h>
#include <lib/component/cpp/service_provider_impl.h>
#include <lib/fit/function.h>
#include <lib/fxl/macros.h>

namespace ledger {

// FakeNetConnector implements NetConnector. It acts as the singleton
// NetConnector for a (virtual) host.
class FakeNetConnector : public fuchsia::netconnector::NetConnector {
 public:
  class Delegate {
   public:
    virtual ~Delegate() {}

    // Returns the list of known devices. See NetConnector::GetKnownDeviceNames
    // for more details.
    virtual void GetDevicesNames(
        uint64_t last_version,
        fit::function<void(uint64_t, std::vector<std::string>)>
            callback) = 0;

    // Connects to the ServiceProvider from host |device_name|.
    virtual void ConnectToServiceProvider(
        std::string device_name,
        fidl::InterfaceRequest<fuchsia::sys::ServiceProvider> request) = 0;
  };

  explicit FakeNetConnector(Delegate* delegate);
  ~FakeNetConnector() override {}

  // Connects to the service provider of this (virtual) host
  void ConnectToServiceProvider(
      fidl::InterfaceRequest<fuchsia::sys::ServiceProvider> request);

 private:
  // NetConnector implementation:
  void RegisterServiceProvider(
      std::string name,
      fidl::InterfaceHandle<fuchsia::sys::ServiceProvider> service_provider)
      override;
  void GetDeviceServiceProvider(
      std::string device_name,
      fidl::InterfaceRequest<fuchsia::sys::ServiceProvider> service_provider)
      override;
  void GetKnownDeviceNames(uint64_t version_last_seen,
                           GetKnownDeviceNamesCallback callback) override;

  component::ServiceProviderImpl service_provider_impl_;
  Delegate* const delegate_;

  FXL_DISALLOW_COPY_AND_ASSIGN(FakeNetConnector);
};

}  // namespace ledger

#endif  // PERIDOT_BIN_LEDGER_TESTING_NETCONNECTOR_FAKE_NETCONNECTOR_H_
