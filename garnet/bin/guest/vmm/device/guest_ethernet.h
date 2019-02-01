// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_BIN_GUEST_VMM_DEVICE_GUEST_ETHERNET_H_
#define GARNET_BIN_GUEST_VMM_DEVICE_GUEST_ETHERNET_H_

#include <fuchsia/hardware/ethernet/cpp/fidl.h>
#include <lib/async/cpp/wait.h>
#include <lib/fit/function.h>

// Interface for GuestEthernet to send a packet to the guest.
struct GuestEthernetReceiver {
  virtual void Receive(uintptr_t addr, size_t length,
                       const fuchsia::hardware::ethernet::FifoEntry& entry) = 0;
};

class GuestEthernet : public fuchsia::hardware::ethernet::Device {
 public:
  using QueueTxFn = fit::function<zx_status_t(
      uintptr_t addr, size_t length,
      const fuchsia::hardware::ethernet::FifoEntry& entry)>;

  GuestEthernet(GuestEthernetReceiver* receiver)
      : tx_fifo_wait_(this), receiver_(receiver) {}

  // Interface for the virtio-net device to send a received packet to the host
  // netstack.
  zx_status_t Send(void* offset, size_t length);

  // Interface for the virtio-net device to inform the netstack that a packet
  // has finished being transmitted.
  void Complete(const fuchsia::hardware::ethernet::FifoEntry& entry);

  void OnTxFifoReadable(async_dispatcher_t* dispatcher, async::WaitBase* wait,
                        zx_status_t status, const zx_packet_signal_t* signal);

  // |fuchsia::hardware::ethernet::Device|
  void GetInfo(GetInfoCallback callback) override;

  // |fuchsia::hardware::ethernet::Device|
  void GetFifos(GetFifosCallback callback) override;

  // |fuchsia::hardware::ethernet::Device|
  void SetIOBuffer(zx::vmo h, SetIOBufferCallback callback) override;

  // |fuchsia::hardware::ethernet::Device|
  void Start(StartCallback callback) override;

  // |fuchsia::hardware::ethernet::Device|
  void Stop(StopCallback callback) override;

  // |fuchsia::hardware::ethernet::Device|
  void ListenStart(ListenStartCallback callback) override;

  // |fuchsia::hardware::ethernet::Device|
  void ListenStop(ListenStopCallback callback) override;

  // |fuchsia::hardware::ethernet::Device|
  void SetClientName(std::string name,
                     SetClientNameCallback callback) override;

  // |fuchsia::hardware::ethernet::Device|
  void GetStatus(GetStatusCallback callback) override;

  // |fuchsia::hardware::ethernet::Device|
  void SetPromiscuousMode(bool enabled,
                          SetPromiscuousModeCallback callback) override;

  // |fuchsia::hardware::ethernet::Device|
  void ConfigMulticastAddMac(fuchsia::hardware::ethernet::MacAddress addr,
                             ConfigMulticastAddMacCallback callback) override;

  // |fuchsia::hardware::ethernet::Device|
  void ConfigMulticastDeleteMac(
      fuchsia::hardware::ethernet::MacAddress addr,
      ConfigMulticastDeleteMacCallback callback) override;

  // |fuchsia::hardware::ethernet::Device|
  void ConfigMulticastSetPromiscuousMode(
      bool enabled,
      ConfigMulticastSetPromiscuousModeCallback callback) override;

  // |fuchsia::hardware::ethernet::Device|
  void ConfigMulticastTestFilter(
      ConfigMulticastTestFilterCallback callback) override;

  // |fuchsia::hardware::ethernet::Device|
  void DumpRegisters(DumpRegistersCallback callback) override;

 private:
  static constexpr uint16_t kVirtioNetQueueSize = 256;
  zx::fifo tx_fifo_;
  zx::fifo rx_fifo_;

  zx::vmo io_vmo_;
  uintptr_t io_addr_;
  size_t io_size_;

  std::vector<fuchsia::hardware::ethernet::FifoEntry> rx_entries_ =
      std::vector<fuchsia::hardware::ethernet::FifoEntry>(kVirtioNetQueueSize);
  size_t rx_entries_count_ = 0;

  async::WaitMethod<GuestEthernet, &GuestEthernet::OnTxFifoReadable>
      tx_fifo_wait_;

  GuestEthernetReceiver* receiver_;
};

#endif  // GARNET_BIN_GUEST_VMM_DEVICE_GUEST_ETHERNET_H_
