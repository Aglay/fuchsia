// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "remote_device.h"

#include "apps/bluetooth/lib/hci/low_energy_scanner.h"
#include "lib/ftl/logging.h"

namespace bluetooth {
namespace gap {

RemoteDevice::RemoteDevice(const std::string& identifier, const common::DeviceAddress& address)
    : identifier_(identifier), address_(address) {
  FTL_DCHECK(!identifier_.empty());

  technology_ = (address_.type() == common::DeviceAddress::Type::kBREDR)
                    ? TechnologyType::kClassic
                    : TechnologyType::kLowEnergy;
}

void RemoteDevice::SetLowEnergyData(bool connectable, int8_t rssi,
                                    const common::ByteBuffer& advertising_data) {
  FTL_DCHECK(technology() == TechnologyType::kLowEnergy);
  FTL_DCHECK(address_.type() != common::DeviceAddress::Type::kBREDR);

  connectable_ = connectable;
  rssi_ = rssi;
  advertising_data_length_ = advertising_data.size();

  // Reallocate the advertising data buffer only if we need more space.
  if (advertising_data_buffer_.size() < advertising_data.size()) {
    advertising_data_buffer_ = common::DynamicByteBuffer(advertising_data.size());
  }

  advertising_data.Copy(&advertising_data_buffer_);
}

}  // namespace gap
}  // namespace bluetooth
