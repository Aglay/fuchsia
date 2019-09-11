// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "msd_qcom_device.h"

#include <magma_util/macros.h>

#include "msd_qcom_platform_device.h"
#include "registers.h"

std::unique_ptr<MsdQcomDevice> MsdQcomDevice::Create(void* device_handle) {
  auto device = std::make_unique<MsdQcomDevice>();

  if (!device->Init(device_handle, nullptr))
    return DRETP(nullptr, "Device init failed");

  return device;
}

bool MsdQcomDevice::Init(void* device_handle, std::unique_ptr<magma::RegisterIo::Hook> hook) {
  qcom_platform_device_ = MsdQcomPlatformDevice::Create(device_handle);
  if (!qcom_platform_device_)
    return DRETF(false, "Failed to create platform device from handle: %p", device_handle);

  std::unique_ptr<magma::PlatformMmio> mmio(qcom_platform_device_->platform_device()->CpuMapMmio(
      0, magma::PlatformMmio::CACHE_POLICY_UNCACHED_DEVICE));
  if (!mmio)
    return DRETF(false, "Failed to map mmio");

  register_io_ = std::make_unique<magma::RegisterIo>(std::move(mmio));
  if (!register_io_)
    return DRETF(false, "Failed to create register io");

  if (hook) {
    register_io_->InstallHook(std::move(hook));
  }

  if (!HardwareInit())
    return DRETF(false, "HardwareInit failed");

  return true;
}

bool MsdQcomDevice::HardwareInit() {
  registers::A6xxRbbmSecvidTsbControl::CreateFrom(0).WriteTo(register_io_.get());

  // Disable trusted memory
  registers::A6xxRbbmSecvidTsbTrustedBase::CreateFrom(0).WriteTo(register_io_.get());
  registers::A6xxRbbmSecvidTsbTrustedSize::CreateFrom(0).WriteTo(register_io_.get());

  if (!EnableClockGating(false))
    return DRETF(false, "EnableHardwareClockGating failed");

  registers::A6xxVbifGateOffWrreqEnable::CreateFrom(0x9).WriteTo(register_io_.get());
  registers::A6xxRbbmVbifClientQosControl::CreateFrom(0x3).WriteTo(register_io_.get());

  // Disable l2 bypass
  registers::A6xxRbbmUcheWriteRangeMax::CreateFrom(0x0001ffffffffffc0).WriteTo(register_io_.get());
  registers::A6xxUcheTrapBase::CreateFrom(0x0001fffffffff000).WriteTo(register_io_.get());
  registers::A6xxUcheWriteThroughBase::CreateFrom(0x0001fffffffff000).WriteTo(register_io_.get());

  registers::A6xxUcheGmemRangeMin::CreateFrom(kGmemGpuAddrBase).WriteTo(register_io_.get());
  registers::A6xxUcheGmemRangeMax::CreateFrom(kGmemGpuAddrBase + GetGmemSize() - 1)
      .WriteTo(register_io_.get());

  registers::A6xxUcheFilterControl::CreateFrom(0x804).WriteTo(register_io_.get());
  registers::A6xxUcheCacheWays::CreateFrom(0x4).WriteTo(register_io_.get());

  registers::A6xxCpRoqThresholds2::CreateFrom(0x010000c0).WriteTo(register_io_.get());
  registers::A6xxCpRoqThresholds1::CreateFrom(0x8040362c).WriteTo(register_io_.get());

  registers::A6xxCpMemPoolSize::CreateFrom(128).WriteTo(register_io_.get());

  registers::A6xxPcDbgEcoControl::CreateFrom(0x300 << 11).WriteTo(register_io_.get());

  // Set AHB default slave response to "ERROR"
  registers::A6xxCpAhbControl::CreateFrom(0x1).WriteTo(register_io_.get());

  registers::A6xxRbbmPerfCounterControl::CreateFrom(0x1).WriteTo(register_io_.get());

  // Always count cycles
  registers::A6xxCpPerfCounterCpSel0::CreateFrom(0).WriteTo(register_io_.get());

  registers::A6xxRbNcModeControl::CreateFrom(2 << 1).WriteTo(register_io_.get());
  registers::A6xxTpl1NcModeControl::CreateFrom(2 << 1).WriteTo(register_io_.get());
  registers::A6xxSpNcModeControl::CreateFrom(2 << 1).WriteTo(register_io_.get());
  registers::A6xxUcheModeControl::CreateFrom(2 << 21).WriteTo(register_io_.get());

  registers::A6xxRbbmInterfaceHangInterruptControl::CreateFrom((1 << 30) | 0x1fffff)
      .WriteTo(register_io_.get());

  registers::A6xxUcheClientPf::CreateFrom(1).WriteTo(register_io_.get());

  // Protect registers from CP
  registers::A6xxCpProtectControl::CreateFrom(0x3).WriteTo(register_io_.get());

  registers::A6xxCpProtect<0>::CreateFrom(
      registers::A6xxCpProtectBase::protect_allow_read(0x600, 0x51))
      .WriteTo(register_io_.get());
  registers::A6xxCpProtect<1>::CreateFrom(registers::A6xxCpProtectBase::protect(0xae50, 0x2))
      .WriteTo(register_io_.get());
  registers::A6xxCpProtect<2>::CreateFrom(registers::A6xxCpProtectBase::protect(0x9624, 0x13))
      .WriteTo(register_io_.get());
  registers::A6xxCpProtect<3>::CreateFrom(registers::A6xxCpProtectBase::protect(0x8630, 0x8))
      .WriteTo(register_io_.get());
  registers::A6xxCpProtect<4>::CreateFrom(registers::A6xxCpProtectBase::protect(0x9e70, 0x1))
      .WriteTo(register_io_.get());
  registers::A6xxCpProtect<5>::CreateFrom(registers::A6xxCpProtectBase::protect(0x9e78, 0x187))
      .WriteTo(register_io_.get());
  registers::A6xxCpProtect<6>::CreateFrom(registers::A6xxCpProtectBase::protect(0xf000, 0x810))
      .WriteTo(register_io_.get());
  registers::A6xxCpProtect<7>::CreateFrom(
      registers::A6xxCpProtectBase::protect_allow_read(0xfc00, 0x3))
      .WriteTo(register_io_.get());
  registers::A6xxCpProtect<8>::CreateFrom(registers::A6xxCpProtectBase::protect(0x50e, 0x0))
      .WriteTo(register_io_.get());
  registers::A6xxCpProtect<9>::CreateFrom(
      registers::A6xxCpProtectBase::protect_allow_read(0x50f, 0x0))
      .WriteTo(register_io_.get());
  registers::A6xxCpProtect<10>::CreateFrom(registers::A6xxCpProtectBase::protect(0x510, 0x0))
      .WriteTo(register_io_.get());
  registers::A6xxCpProtect<11>::CreateFrom(
      registers::A6xxCpProtectBase::protect_allow_read(0x0, 0x4f9))
      .WriteTo(register_io_.get());
  registers::A6xxCpProtect<12>::CreateFrom(
      registers::A6xxCpProtectBase::protect_allow_read(0x501, 0xa))
      .WriteTo(register_io_.get());
  registers::A6xxCpProtect<13>::CreateFrom(
      registers::A6xxCpProtectBase::protect_allow_read(0x511, 0x44))
      .WriteTo(register_io_.get());
  registers::A6xxCpProtect<14>::CreateFrom(registers::A6xxCpProtectBase::protect(0xe00, 0xe))
      .WriteTo(register_io_.get());
  registers::A6xxCpProtect<15>::CreateFrom(registers::A6xxCpProtectBase::protect(0x8e00, 0x0))
      .WriteTo(register_io_.get());
  registers::A6xxCpProtect<16>::CreateFrom(registers::A6xxCpProtectBase::protect(0x8e50, 0xf))
      .WriteTo(register_io_.get());
  registers::A6xxCpProtect<17>::CreateFrom(registers::A6xxCpProtectBase::protect(0xbe02, 0x0))
      .WriteTo(register_io_.get());
  registers::A6xxCpProtect<18>::CreateFrom(registers::A6xxCpProtectBase::protect(0xbe20, 0x11f3))
      .WriteTo(register_io_.get());
  registers::A6xxCpProtect<19>::CreateFrom(registers::A6xxCpProtectBase::protect(0x800, 0x82))
      .WriteTo(register_io_.get());
  registers::A6xxCpProtect<20>::CreateFrom(registers::A6xxCpProtectBase::protect(0x8a0, 0x8))
      .WriteTo(register_io_.get());
  registers::A6xxCpProtect<21>::CreateFrom(registers::A6xxCpProtectBase::protect(0x8ab, 0x19))
      .WriteTo(register_io_.get());
  registers::A6xxCpProtect<22>::CreateFrom(registers::A6xxCpProtectBase::protect(0x900, 0x4d))
      .WriteTo(register_io_.get());
  registers::A6xxCpProtect<23>::CreateFrom(registers::A6xxCpProtectBase::protect(0x98d, 0x76))
      .WriteTo(register_io_.get());
  registers::A6xxCpProtect<24>::CreateFrom(
      registers::A6xxCpProtectBase::protect_allow_read(0x980, 0x4))
      .WriteTo(register_io_.get());
  registers::A6xxCpProtect<25>::CreateFrom(registers::A6xxCpProtectBase::protect(0xa630, 0x0))
      .WriteTo(register_io_.get());

  return true;
}

bool MsdQcomDevice::EnableClockGating(bool enable) {
  uint32_t val = registers::A6xxRbbmClockControl::CreateFrom(register_io_.get()).reg_value();
  if (!enable && val == 0)
    return true;

  return DRETF(false, "EnableClockGating: not implemented: enable %d val 0x%x", enable, val);
}
