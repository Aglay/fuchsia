// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_BIN_GUEST_INTEGRATION_MOCK_NETSTACK_H_
#define GARNET_BIN_GUEST_INTEGRATION_MOCK_NETSTACK_H_

#include <fuchsia/netstack/cpp/fidl.h>
#include <lib/fidl/cpp/binding_set.h>

static constexpr size_t kVmoSize = 1024;
static constexpr zx::duration kTestTimeout = zx::sec(15);

class MockNetstack : public fuchsia::netstack::Netstack {
 public:
  void GetPortForService(std::string service,
                         fuchsia::netstack::Protocol protocol,
                         GetPortForServiceCallback callback) override {}

  void GetAddress(std::string address, uint16_t port,
                  GetAddressCallback callback) override {}

  void GetInterfaces(GetInterfacesCallback callback) override {}

  void GetRouteTable(GetRouteTableCallback callback) override {}

  void GetStats(uint32_t nicid, GetStatsCallback callback) override {}

  void GetAggregateStats(GetAggregateStatsCallback callback) override {}

  void SetInterfaceStatus(uint32_t nicid, bool enabled) override {}

  void SetInterfaceAddress(uint32_t nicid, fuchsia::net::IpAddress addr,
                           uint8_t prefixLen,
                           SetInterfaceAddressCallback callback) override {}

  void RemoveInterfaceAddress(
      uint32_t nicid, fuchsia::net::IpAddress addr, uint8_t prefixLen,
      RemoveInterfaceAddressCallback callback) override {}

  void SetDhcpClientStatus(uint32_t nicid, bool enabled,
                           SetDhcpClientStatusCallback callback) override {}

  void BridgeInterfaces(std::vector<uint32_t> nicids,
                        BridgeInterfacesCallback callback) override {}

  void SetNameServers(std::vector<fuchsia::net::IpAddress> servers) override {}

  void AddEthernetDevice(
      std::string topological_path,
      fuchsia::netstack::InterfaceConfig interfaceConfig,
      ::fidl::InterfaceHandle<::fuchsia::hardware::ethernet::Device> device,
      AddEthernetDeviceCallback callback) override;

  void StartRouteTableTransaction(
      ::fidl::InterfaceRequest<fuchsia::netstack::RouteTableTransaction>
          routeTableTransaction,
      StartRouteTableTransactionCallback callback) override {}

  fidl::InterfaceRequestHandler<fuchsia::netstack::Netstack> GetHandler() {
    return bindings_.GetHandler(this);
  }

  zx_status_t SendPacket(void* packet, size_t length);
  zx_status_t ReceivePacket(void* packet, size_t length);

 private:
  fidl::BindingSet<fuchsia::netstack::Netstack> bindings_;
  fuchsia::hardware::ethernet::DeviceSyncPtr eth_device_;

  zx::fifo rx_;
  zx::fifo tx_;
  zx::vmo vmo_;
  uintptr_t io_addr_;
};

#endif  // GARNET_BIN_GUEST_INTEGRATION_MOCK_NETSTACK_H_