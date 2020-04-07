// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_DRIVERS_AMLOGIC_DECODER_DECODER_CORE_H_
#define SRC_MEDIA_DRIVERS_AMLOGIC_DECODER_DECODER_CORE_H_

#include <lib/zx/handle.h>

#include <ddk/io-buffer.h>

#include "internal_buffer.h"
#include "memory_barriers.h"
#include "registers.h"

struct MmioRegisters {
  DosRegisterIo* dosbus;
  AoRegisterIo* aobus;
  DmcRegisterIo* dmc;
  HiuRegisterIo* hiubus;
  ResetRegisterIo* reset;
  ParserRegisterIo* parser;
  DemuxRegisterIo* demux;
};

struct InputContext {
  ~InputContext() {
    BarrierBeforeRelease();
    // ~buffer
  }

  std::optional<InternalBuffer> buffer;

  uint32_t processed_video = 0;
};

enum class DeviceType;

enum class ClockType {
  kGclkVdec,
  kMax,
};

class DecoderCore {
 public:
  class Owner {
   public:
    [[nodiscard]] virtual zx::unowned_bti bti() = 0;

    [[nodiscard]] virtual MmioRegisters* mmio() = 0;

    virtual void UngateClocks() = 0;

    virtual void GateClocks() = 0;

    virtual void ToggleClock(ClockType type, bool enable) = 0;

    [[nodiscard]] virtual DeviceType device_type() = 0;

    [[nodiscard]] virtual fuchsia::sysmem::AllocatorSyncPtr& SysmemAllocatorSyncPtr() = 0;
  };

  virtual ~DecoderCore() {}

  virtual __WARN_UNUSED_RESULT zx_status_t LoadFirmware(const uint8_t* data, uint32_t len) = 0;
  virtual void PowerOn() = 0;
  virtual void PowerOff() = 0;
  virtual void StartDecoding() = 0;
  virtual void StopDecoding() = 0;
  virtual void WaitForIdle() = 0;
  virtual void InitializeStreamInput(bool use_parser, uint32_t buffer_address,
                                     uint32_t buffer_size) = 0;
  virtual void InitializeParserInput() = 0;
  virtual void InitializeDirectInput() = 0;
  // The write pointer points to just after the last thing that was written into
  // the stream buffer.
  virtual void UpdateWritePointer(uint32_t write_pointer) = 0;
  // This is the offset between the start of the stream buffer and the write
  // pointer.
  [[nodiscard]] virtual uint32_t GetStreamInputOffset() = 0;
  [[nodiscard]] virtual uint32_t GetReadOffset() = 0;

  [[nodiscard]] virtual zx_status_t InitializeInputContext(InputContext* context, bool is_secure) {
    return ZX_ERR_NOT_SUPPORTED;
  }
  virtual void SaveInputContext(InputContext* context) {}
  virtual void RestoreInputContext(InputContext* context) {}
};

#endif  // SRC_MEDIA_DRIVERS_AMLOGIC_DECODER_DECODER_CORE_H_
