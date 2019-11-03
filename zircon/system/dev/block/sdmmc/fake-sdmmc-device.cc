// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <hw/sdmmc.h>
#include <hw/sdio.h>

#include "fake-sdmmc-device.h"

namespace sdmmc {

zx_status_t FakeSdmmcDevice::SdmmcHostInfo(sdmmc_host_info_t* out_info) {
  memcpy(out_info, &host_info_, sizeof(host_info_));
  return ZX_OK;
}

zx_status_t FakeSdmmcDevice::SdmmcRequest(sdmmc_req_t* req) {
  command_counts_[req->cmd_idx]++;

  uint8_t* const virt_buffer = reinterpret_cast<uint8_t*>(req->virt_buffer) + req->buf_offset;

  req->response[0] = 0;
  req->response[1] = 0;
  req->response[2] = 0;
  req->response[3] = 0;

  switch (req->cmd_idx) {
    case SDMMC_READ_BLOCK:
    case SDMMC_READ_MULTIPLE_BLOCK: {
      const size_t req_size = req->blockcount * req->blocksize;
      if ((req->arg & kBadRegionMask) == kBadRegionStart) {
        return ZX_ERR_IO;
      }

      memcpy(virt_buffer, Read(req->arg * kBlockSize, req_size).data(), req_size);
      break;
    }
    case SDMMC_WRITE_BLOCK:
    case SDMMC_WRITE_MULTIPLE_BLOCK: {
      const size_t req_size = req->blockcount * req->blocksize;
      if ((req->arg & kBadRegionMask) == kBadRegionStart) {
        return ZX_ERR_IO;
      }

      Write(req->arg * kBlockSize, fbl::Span<const uint8_t>(virt_buffer, req_size));
      break;
    }
    case SDIO_IO_RW_DIRECT: {
      const uint32_t address =
          (req->arg & SDIO_IO_RW_DIRECT_REG_ADDR_MASK) >> SDIO_IO_RW_DIRECT_REG_ADDR_LOC;
      const uint8_t function =
          (req->arg & SDIO_IO_RW_DIRECT_FN_IDX_MASK) >> SDIO_IO_RW_DIRECT_FN_IDX_LOC;
      if (req->arg & SDIO_IO_RW_DIRECT_RW_FLAG) {
        Write(address,
              std::vector{static_cast<uint8_t>(req->arg & SDIO_IO_RW_DIRECT_WRITE_BYTE_MASK)},
              function);
      } else {
        req->response[0] = Read(address, 1, function)[0];
      }
      break;
    }
    case SDIO_IO_RW_DIRECT_EXTENDED: {
      const uint32_t address =
          (req->arg & SDIO_IO_RW_EXTD_REG_ADDR_MASK) >> SDIO_IO_RW_EXTD_REG_ADDR_LOC;
      const uint8_t function =
          (req->arg & SDIO_IO_RW_EXTD_FN_IDX_MASK) >> SDIO_IO_RW_EXTD_FN_IDX_LOC;
      const uint32_t block_mode = req->arg & SDIO_IO_RW_EXTD_BLOCK_MODE;
      const uint32_t blocks = req->arg & SDIO_IO_RW_EXTD_BYTE_BLK_COUNT_MASK;
      const std::vector<uint8_t> block_size_reg = Read(0x10 | (function << 8), 2, 0);
      const uint32_t block_size = block_size_reg[0] | (block_size_reg[1] << 8);
      const uint32_t transfer_size =
          block_mode ? (block_size * blocks) : (blocks == 0 ? 512 : blocks);
      if (req->arg & SDIO_IO_RW_DIRECT_RW_FLAG) {
        Write(address, fbl::Span<const uint8_t>(virt_buffer, transfer_size), function);
      } else {
        memcpy(virt_buffer, Read(address, transfer_size, function).data(), transfer_size);
      }
    }
    default:
      break;
  }

  req->status = ZX_OK;

  if (command_callbacks_.find(req->cmd_idx) != command_callbacks_.end()) {
    command_callbacks_[req->cmd_idx](req);
  }

  return req->status;
}

zx_status_t FakeSdmmcDevice::SdmmcRegisterInBandInterrupt(
    const in_band_interrupt_protocol_t* interrupt_cb) {
  interrupt_cb_ = *interrupt_cb;
  return ZX_OK;
}

std::vector<uint8_t> FakeSdmmcDevice::Read(size_t address, size_t size, uint8_t func) {
  std::map<size_t, std::unique_ptr<uint8_t[]>>& sectors = sectors_[func];

  std::vector<uint8_t> ret;
  size_t start = address;
  for (; start < address + size; start = (start & kBlockMask) + kBlockSize) {
    if (sectors.find(start & kBlockMask) == sectors.end()) {
      sectors[start & kBlockMask].reset(new uint8_t[kBlockSize]);
      memset(sectors[start & kBlockMask].get(), 0xff, kBlockSize);
    }

    const size_t read_offset = start - (start & kBlockMask);
    const size_t read_size = std::min(kBlockSize - read_offset, size - start + address);
    const uint8_t* const read_ptr = sectors[start & kBlockMask].get() + read_offset;
    ret.insert(ret.end(), read_ptr, read_ptr + read_size);
  }

  return ret;
}

void FakeSdmmcDevice::Write(size_t address, fbl::Span<const uint8_t> data, uint8_t func) {
  std::map<size_t, std::unique_ptr<uint8_t[]>>& sectors = sectors_[func];

  const uint8_t* data_ptr = data.data();
  size_t start = address;
  for (; start < address + data.size(); start = (start & kBlockMask) + kBlockSize) {
    if (sectors.find(start & kBlockMask) == sectors.end()) {
      sectors[start & kBlockMask].reset(new uint8_t[kBlockSize]);
      memset(sectors[start & kBlockMask].get(), 0xff, kBlockSize);
    }

    const size_t write_offset = start - (start & kBlockMask);
    const size_t write_size = std::min(kBlockSize - write_offset, data.size() - start + address);
    memcpy(sectors[start & kBlockMask].get() + write_offset, data_ptr, write_size);

    data_ptr += write_size;
  }
}

void FakeSdmmcDevice::TriggerInBandInterrupt() { interrupt_cb_.ops->callback(interrupt_cb_.ctx); }

}  // namespace sdmmc
