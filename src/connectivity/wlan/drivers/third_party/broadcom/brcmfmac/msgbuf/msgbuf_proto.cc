// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/msgbuf/msgbuf_proto.h"

#include <zircon/errors.h>
#include <zircon/status.h>
#include <zircon/syscalls.h>

#include <algorithm>
#include <cstring>

#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/debug.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/dma_buffer.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/dma_pool.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/msgbuf/msgbuf_ring_handler.h"

namespace wlan {
namespace brcmfmac {
namespace {

// Size and count of RX buffers.
constexpr size_t kRxBufferSize = 2048;
constexpr int kRxBufferCount = 2048;

// Size and count of TX buffers.
constexpr size_t kTxBufferSize = 2048;
constexpr int kTxBufferCount = 2048;

// Create a brcmf_proto instance with the appropriate trampolines.
std::unique_ptr<brcmf_proto> CreateProto(MsgbufProto* msgbuf) {
  auto proto = std::make_unique<brcmf_proto>();

  proto->hdrpull = [](brcmf_pub* drvr, bool do_fws, brcmf_netbuf* netbuf, brcmf_if** ifp) {
    return reinterpret_cast<MsgbufProto*>(drvr->proto->pd)->HdrPull(do_fws, netbuf, ifp);
  };
  proto->query_dcmd = [](brcmf_pub* drvr, int ifidx, uint cmd, void* buf, uint len,
                         int32_t* fwerr) {
    return reinterpret_cast<MsgbufProto*>(drvr->proto->pd)->QueryDcmd(ifidx, cmd, buf, len, fwerr);
  };
  proto->set_dcmd = [](brcmf_pub* drvr, int ifidx, uint cmd, void* buf, uint len, int32_t* fwerr) {
    return reinterpret_cast<MsgbufProto*>(drvr->proto->pd)->SetDcmd(ifidx, cmd, buf, len, fwerr);
  };
  proto->tx_queue_data = [](brcmf_pub* drvr, int ifidx, std::unique_ptr<Netbuf> netbuf) {
    return reinterpret_cast<MsgbufProto*>(drvr->proto->pd)->TxQueueData(ifidx, std::move(netbuf));
  };
  proto->configure_addr_mode = [](brcmf_pub* drvr, int ifidx, proto_addr_mode addr_mode) {
    return reinterpret_cast<MsgbufProto*>(drvr->proto->pd)->ConfigureAddrMode(ifidx, addr_mode);
  };
  proto->delete_peer = [](brcmf_pub* drvr, int ifidx, uint8_t peer[ETH_ALEN]) {
    return reinterpret_cast<MsgbufProto*>(drvr->proto->pd)->DeletePeer(ifidx, peer);
  };
  proto->add_tdls_peer = [](brcmf_pub* drvr, int ifidx, uint8_t peer[ETH_ALEN]) {
    return reinterpret_cast<MsgbufProto*>(drvr->proto->pd)->AddTdlsPeer(ifidx, peer);
  };
  proto->rxreorder = [](brcmf_if* ifp, brcmf_netbuf* netbuf) {
    return reinterpret_cast<MsgbufProto*>(ifp->drvr->proto->pd)->RxReorder(netbuf);
  };

  proto->pd = msgbuf;
  return proto;
}

// Create a DMA pool.
zx_status_t CreateDmaPool(DmaBufferProviderInterface* dma_buffer_provider, size_t buffer_size,
                          int buffer_count, std::unique_ptr<DmaPool>* dma_pool_out) {
  zx_status_t status = ZX_OK;
  std::unique_ptr<DmaBuffer> dma_buffer;
  if ((status = dma_buffer_provider->CreateDmaBuffer(
           ZX_CACHE_POLICY_CACHED, buffer_count * buffer_size, &dma_buffer)) != ZX_OK) {
    return status;
  }

  std::unique_ptr<DmaPool> dma_pool;
  if ((status = DmaPool::Create(buffer_size, buffer_count, std::move(dma_buffer), &dma_pool)) !=
      ZX_OK) {
    return status;
  }

  *dma_pool_out = std::move(dma_pool);
  return ZX_OK;
}

}  // namespace

MsgbufProto::MsgbufProto() = default;

MsgbufProto::~MsgbufProto() {
  if (device_ != nullptr) {
    if (device_->drvr()->proto == proto_.get()) {
      device_->drvr()->proto = nullptr;
    }
  }
}

// static
zx_status_t MsgbufProto::Create(Device* device, DmaBufferProviderInterface* dma_buffer_provider,
                                DmaRingProviderInterface* dma_ring_provider,
                                InterruptProviderInterface* interrupt_provider,
                                std::unique_ptr<MsgbufProto>* out_msgbuf) {
  zx_status_t status = ZX_OK;

  auto msgbuf = std::make_unique<MsgbufProto>();
  auto proto = CreateProto(msgbuf.get());

  std::unique_ptr<DmaPool> rx_buffer_pool;
  if ((status = CreateDmaPool(dma_buffer_provider, kRxBufferSize, kRxBufferCount,
                              &rx_buffer_pool)) != ZX_OK) {
    BRCMF_ERR("Failed to create rx buffer pool: %s", zx_status_get_string(status));
    return status;
  }
  std::unique_ptr<DmaPool> tx_buffer_pool;
  if ((status = CreateDmaPool(dma_buffer_provider, kTxBufferSize, kTxBufferCount,
                              &tx_buffer_pool)) != ZX_OK) {
    BRCMF_ERR("Failed to create tx buffer pool: %s", zx_status_get_string(status));
    return status;
  }

  std::unique_ptr<MsgbufRingHandler> ring_handler;
  if ((status = MsgbufRingHandler::Create(dma_ring_provider, interrupt_provider,
                                          std::move(rx_buffer_pool), std::move(tx_buffer_pool),
                                          &ring_handler)) != ZX_OK) {
    BRCMF_ERR("Failed to create MsgbufRingHandler: %s", zx_status_get_string(status));
    return status;
  }

  device->drvr()->proto = proto.get();

  msgbuf->device_ = device;
  msgbuf->proto_ = std::move(proto);
  msgbuf->ring_handler_ = std::move(ring_handler);
  *out_msgbuf = std::move(msgbuf);
  return ZX_OK;
}

zx_status_t MsgbufProto::HdrPull(bool do_fws, brcmf_netbuf* netbuf, brcmf_if** ifp) {
  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t MsgbufProto::QueryDcmd(int ifidx, uint cmd, void* buf, uint len, int32_t* fwerr) {
  zx_status_t status = ZX_OK;

  DmaPool::Buffer tx_buffer;
  if ((status = ring_handler_->GetTxBuffer(&tx_buffer)) != ZX_OK) {
    return status;
  }

  void* tx_buffer_data = nullptr;
  if ((status = tx_buffer.MapWrite(len, &tx_buffer_data)) != ZX_OK) {
    return status;
  }
  std::memcpy(tx_buffer_data, buf, len);

  DmaPool::Buffer rx_buffer;
  size_t rx_data_size = 0;
  int32_t firmware_error = 0;
  status = ring_handler_->Ioctl(ifidx, cmd, std::move(tx_buffer), len, &rx_buffer, &rx_data_size,
                                &firmware_error);
  if (status != ZX_OK) {
    BRCMF_ERR("ioctl failed, ifidx=%d cmd=0x%08x fwerr=%d: %s", ifidx, cmd, firmware_error,
              zx_status_get_string(status));
    return status;
  }

  const size_t read_size = std::min<size_t>(rx_data_size, len);
  const void* rx_buffer_data = nullptr;
  if ((status = rx_buffer.MapRead(read_size, &rx_buffer_data)) != ZX_OK) {
    return status;
  }

  *fwerr = firmware_error;
  std::memcpy(buf, rx_buffer_data, read_size);
  return ZX_OK;
}

zx_status_t MsgbufProto::SetDcmd(int ifidx, uint cmd, void* buf, uint len, int32_t* fwerr) {
  return QueryDcmd(ifidx, cmd, buf, len, fwerr);
}

zx_status_t MsgbufProto::TxQueueData(int ifidx, std::unique_ptr<Netbuf> netbuf) {
  BRCMF_ERR("MsgbufProto::TxQueueData unimplemented");
  netbuf->Return(ZX_ERR_NOT_SUPPORTED);
  return ZX_ERR_NOT_SUPPORTED;
}

void MsgbufProto::ConfigureAddrMode(int ifidx, proto_addr_mode addr_mode) {
  BRCMF_ERR("MsgbufProto::ConfigureAddrMode unimplemented");
}

void MsgbufProto::DeletePeer(int ifidx, uint8_t peer[ETH_ALEN]) {
  BRCMF_ERR("MsgbufProto::DeletePeer unimplemented");
}

void MsgbufProto::AddTdlsPeer(int ifidx, uint8_t peer[ETH_ALEN]) {
  BRCMF_ERR("MsgbufProto::AddTdlsPeer unimplemented");
}

void MsgbufProto::RxReorder(brcmf_netbuf* netbuf) {}

brcmf_proto* MsgbufProto::GetProto() { return proto_.get(); }

}  // namespace brcmfmac
}  // namespace wlan
