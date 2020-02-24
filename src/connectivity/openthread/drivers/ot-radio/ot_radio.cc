// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ot_radio.h"

#include <ctype.h>
#include <lib/async-loop/default.h>
#include <lib/async/cpp/task.h>
#include <lib/driver-unit-test/utils.h>
#include <lib/fidl-async/cpp/async_bind.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <zircon/compiler.h>
#include <zircon/status.h>
#include <zircon/time.h>

#include <iostream>

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/driver.h>
#include <ddk/metadata.h>
#include <ddk/platform-defs.h>
#include <ddk/protocol/composite.h>
#include <ddktl/fidl.h>
#include <fbl/algorithm.h>
#include <fbl/auto_call.h>
#include <fbl/auto_lock.h>
#include <fbl/ref_counted.h>
#include <fbl/ref_ptr.h>
#include <hw/arch_ops.h>
#include <hw/reg.h>

#include "ot_radio_bootloader.h"

namespace ot {
namespace lowpan_spinel_fidl = ::llcpp::fuchsia::lowpan::spinel;

enum {
  COMPONENT_PDEV,
  COMPONENT_SPI,
  COMPONENT_INT_GPIO,
  COMPONENT_RESET_GPIO,
  COMPONENT_BOOTLOADER_GPIO,
  COMPONENT_COUNT,
};

OtRadioDevice::LowpanSpinelDeviceFidlImpl::LowpanSpinelDeviceFidlImpl(OtRadioDevice& ot_radio)
    : ot_radio_obj_(ot_radio) {}

void OtRadioDevice::LowpanSpinelDeviceFidlImpl::Bind(async_dispatcher_t* dispatcher,
                                                     zx::channel channel) {
  ot_radio_obj_.fidl_channel_ = zx::unowned_channel(channel);
  fidl::OnUnboundFn<LowpanSpinelDeviceFidlImpl> on_unbound =
      [](LowpanSpinelDeviceFidlImpl* server, fidl::UnboundReason, zx::channel channel) {
        server->ot_radio_obj_.fidl_channel_ = zx::unowned_channel(ZX_HANDLE_INVALID);
        server->ot_radio_obj_.fidl_impl_obj_.release();
      };
  fidl::AsyncBind(dispatcher, std::move(channel), this, std::move(on_unbound));
}

void OtRadioDevice::LowpanSpinelDeviceFidlImpl::Open(OpenCompleter::Sync completer) {
  zx_status_t res = ot_radio_obj_.Reset();
  if (res == ZX_OK) {
    zxlogf(TRACE, "open succeed, returning\n");
    ot_radio_obj_.power_status_ = OT_SPINEL_DEVICE_ON;
    lowpan_spinel_fidl::Device::SendOnReadyForSendFramesEvent(ot_radio_obj_.fidl_channel_->borrow(),
                                                              kOutboundAllowanceInit);
    ot_radio_obj_.inbound_allowance_ = 0;
    ot_radio_obj_.outbound_allowance_ = kOutboundAllowanceInit;
    ot_radio_obj_.inbound_cnt_ = 0;
    ot_radio_obj_.outbound_cnt_ = 0;
    completer.ReplySuccess();
  } else {
    zxlogf(ERROR, "Error in handling FIDL close req: %s, power status: %u\n",
           zx_status_get_string(res), ot_radio_obj_.power_status_);
    completer.ReplyError(lowpan_spinel_fidl::Error::UNSPECIFIED);
  }
}

void OtRadioDevice::LowpanSpinelDeviceFidlImpl::Close(CloseCompleter::Sync completer) {
  zx_status_t res = ot_radio_obj_.AssertResetPin();
  if (res == ZX_OK) {
    ot_radio_obj_.power_status_ = OT_SPINEL_DEVICE_OFF;
    completer.ReplySuccess();
  } else {
    zxlogf(ERROR, "Error in handling FIDL close req: %s, power status: %u\n",
           zx_status_get_string(res), ot_radio_obj_.power_status_);
    completer.ReplyError(lowpan_spinel_fidl::Error::UNSPECIFIED);
  }
}

void OtRadioDevice::LowpanSpinelDeviceFidlImpl::GetMaxFrameSize(
    GetMaxFrameSizeCompleter::Sync completer) {
  completer.Reply(kMaxFrameSize);
}

void OtRadioDevice::LowpanSpinelDeviceFidlImpl::SendFrame(::fidl::VectorView<uint8_t> data,
                                                          SendFrameCompleter::Sync completer) {
  if (ot_radio_obj_.power_status_ == OT_SPINEL_DEVICE_OFF) {
    lowpan_spinel_fidl::Device::SendOnErrorEvent(ot_radio_obj_.fidl_channel_->borrow(),
                                                 lowpan_spinel_fidl::Error::CLOSED, false);
  } else if (data.count() > kMaxFrameSize) {
    lowpan_spinel_fidl::Device::SendOnErrorEvent(
        ot_radio_obj_.fidl_channel_->borrow(), lowpan_spinel_fidl::Error::OUTBOUND_FRAME_TOO_LARGE,
        false);
  } else if (ot_radio_obj_.outbound_allowance_ == 0) {
    // Client violates the protocol, close FIDL channel and device. Will not send OnError event.
    ot_radio_obj_.fidl_channel_ = zx::unowned_channel(ZX_HANDLE_INVALID);
    ot_radio_obj_.power_status_ = OT_SPINEL_DEVICE_OFF;
    ot_radio_obj_.AssertResetPin();
    ot_radio_obj_.fidl_impl_obj_.release();
    completer.Close(ZX_ERR_IO_OVERRUN);
  } else {
    // All good, send out the frame.
    zx_status_t res = ot_radio_obj_.RadioPacketTx(data.begin(), data.count());
    if (res != ZX_OK) {
      zxlogf(ERROR, "Error in handling send frame req: %s\n", zx_status_get_string(res));
    } else {
      ot_radio_obj_.outbound_allowance_--;
      ot_radio_obj_.outbound_cnt_++;
      zxlogf(TRACE, "Successfully Txed pkt, total tx pkt %lu\n", ot_radio_obj_.outbound_cnt_);
      if ((ot_radio_obj_.outbound_cnt_ & 1) == 0) {
        lowpan_spinel_fidl::Device::SendOnReadyForSendFramesEvent(
            ot_radio_obj_.fidl_channel_->borrow(), kOutboundAllowanceInc);
        ot_radio_obj_.outbound_allowance_ += kOutboundAllowanceInc;
      }
    }
  }
}

void OtRadioDevice::LowpanSpinelDeviceFidlImpl::ReadyToReceiveFrames(
    uint32_t number_of_frames, ReadyToReceiveFramesCompleter::Sync completer) {
  zxlogf(TRACE, "allow to receive %u frame\n", number_of_frames);
  ot_radio_obj_.inbound_allowance_ += number_of_frames;
  if (ot_radio_obj_.inbound_allowance_ > 0 && ot_radio_obj_.spinel_framer_.get()) {
    ot_radio_obj_.spinel_framer_->SetInboundAllowanceStatus(true);
    ot_radio_obj_.ReadRadioPacket();
  }
}

OtRadioDevice::OtRadioDevice(zx_device_t* device)
    : ddk::Device<OtRadioDevice, ddk::UnbindableNew, ddk::Messageable>(device),
      loop_(&kAsyncLoopConfigNoAttachToCurrentThread) {}

zx_status_t OtRadioDevice::DdkMessage(fidl_msg_t* msg, fidl_txn_t* txn) {
  DdkTransaction transaction(txn);
  lowpan_spinel_fidl::DeviceSetup::Dispatch(this, msg, &transaction);
  return transaction.Status();
}

void OtRadioDevice::SetChannel(zx::channel channel, SetChannelCompleter::Sync completer) {
  if (fidl_impl_obj_ != nullptr) {
    zxlogf(ERROR, "ot-audio: channel already set\n");
    completer.ReplyError(ZX_ERR_ALREADY_BOUND);
    return;
  }
  if (!channel.is_valid()) {
    completer.ReplyError(ZX_ERR_BAD_HANDLE);
    return;
  }
  fidl_impl_obj_ = std::make_unique<LowpanSpinelDeviceFidlImpl>(*this);
  fidl_impl_obj_->Bind(loop_.dispatcher(), std::move(channel));
  completer.ReplySuccess();
}

zx_status_t OtRadioDevice::StartLoopThread() {
  zxlogf(TRACE, "Start loop thread\n");
  zx_status_t status = loop_.StartThread("ot-stack-loop");
  if (status == ZX_OK) {
    thrd_status_.loop_thrd_running = true;
  }
  return status;
}

bool OtRadioDevice::RunUnitTests(void* ctx, zx_device_t* parent, zx_handle_t channel) {
  return driver_unit_test::RunZxTests("OtRadioTests", parent, channel);
}

zx_status_t OtRadioDevice::Init() {
  composite_protocol_t composite;

  auto status = device_get_protocol(parent(), ZX_PROTOCOL_COMPOSITE, &composite);
  if (status != ZX_OK) {
    zxlogf(ERROR, "Could not get composite protocol\n");
    return status;
  }

  zx_device_t* components[COMPONENT_COUNT];
  size_t actual;
  composite_get_components(&composite, components, fbl::count_of(components), &actual);
  if (actual != fbl::count_of(components)) {
    zxlogf(ERROR, "could not get components\n");
    return ZX_ERR_NOT_SUPPORTED;
  }

  spi_ = ddk::SpiProtocolClient(components[COMPONENT_SPI]);
  if (!spi_.is_valid()) {
    zxlogf(ERROR, "ot-radio %s: failed to acquire spi\n", __func__);
    return ZX_ERR_NOT_SUPPORTED;
  }

  status = device_get_protocol(components[COMPONENT_INT_GPIO], ZX_PROTOCOL_GPIO,
                               &gpio_[OT_RADIO_INT_PIN]);
  if (status != ZX_OK) {
    zxlogf(ERROR, "ot-radio %s: failed to acquire interrupt gpio\n", __func__);
    return status;
  }

  status = gpio_[OT_RADIO_INT_PIN].ConfigIn(GPIO_NO_PULL);

  if (status != ZX_OK) {
    zxlogf(ERROR, "ot-radio %s: failed to configure interrupt gpio\n", __func__);
    return status;
  }

  status = gpio_[OT_RADIO_INT_PIN].GetInterrupt(ZX_INTERRUPT_MODE_EDGE_LOW, &interrupt_);

  if (status != ZX_OK) {
    zxlogf(ERROR, "ot-radio %s: failed to get interrupt\n", __func__);
    return status;
  }

  status = device_get_protocol(components[COMPONENT_RESET_GPIO], ZX_PROTOCOL_GPIO,
                               &gpio_[OT_RADIO_RESET_PIN]);
  if (status != ZX_OK) {
    zxlogf(ERROR, "ot-radio %s: failed to acquire reset gpio\n", __func__);
    return status;
  }

  status = gpio_[OT_RADIO_RESET_PIN].ConfigOut(1);

  if (status != ZX_OK) {
    zxlogf(ERROR, "ot-radio %s: failed to configure rst gpio, status = %d", __func__, status);
    return status;
  }

  status = device_get_protocol(components[COMPONENT_BOOTLOADER_GPIO], ZX_PROTOCOL_GPIO,
                               &gpio_[OT_RADIO_BOOTLOADER_PIN]);
  if (status != ZX_OK) {
    zxlogf(ERROR, "ot-radio %s: failed to acquire radio bootloader pin\n", __func__);
    return status;
  }

  status = gpio_[OT_RADIO_BOOTLOADER_PIN].ConfigOut(1);

  if (status != ZX_OK) {
    zxlogf(ERROR, "ot-radio %s: failed to configure bootloader gpio, status = %d", __func__,
           status);
    return status;
  }

  uint32_t device_id;
  status = device_get_metadata(components[COMPONENT_PDEV], DEVICE_METADATA_PRIVATE, &device_id,
                               sizeof(device_id), &actual);
  if (status != ZX_OK || sizeof(device_id) != actual) {
    zxlogf(ERROR, "ot-radio: failed to read metadata\n");
    return status == ZX_OK ? ZX_ERR_INTERNAL : status;
  }

  spinel_framer_ = std::make_unique<ot::SpinelFramer>();
  spinel_framer_->Init(spi_);

  return ZX_OK;
}

zx_status_t OtRadioDevice::ReadRadioPacket() {
  if ((inbound_allowance_ > 0) && (spinel_framer_->IsPacketPresent())) {
    spinel_framer_->ReceivePacketFromRadio(spi_rx_buffer_, &spi_rx_buffer_len_);
    if (spi_rx_buffer_len_ > 0) {
      async::PostTask(loop_.dispatcher(), [this, pkt = std::move(spi_rx_buffer_),
                                           len = std::move(spi_rx_buffer_len_)]() {
        this->HandleRadioRxFrame(pkt, len);
      });

      // Signal to driver test, waiting for a response
      sync_completion_signal(&spi_rx_complete_);
    }
  }
  return ZX_OK;
}

zx_status_t OtRadioDevice::HandleRadioRxFrame(uint8_t* frameBuffer, uint16_t length) {
  zxlogf(INFO, "ot-radio: received frame of len:%d\n", length);
  if (power_status_ == OT_SPINEL_DEVICE_ON) {
    ::fidl::VectorView<uint8_t> data;
    data.set_count(length);
    data.set_data(frameBuffer);
    zx_status_t res = lowpan_spinel_fidl::Device::SendOnReceiveFrameEvent(fidl_channel_->borrow(),
                                                                          std::move(data));
    if (res != ZX_OK) {
      zxlogf(ERROR, "ot-radio: failed to send OnReceive() event due to %s\n",
             zx_status_get_string(res));
    }
    inbound_allowance_--;
    inbound_cnt_++;
    if ((inbound_allowance_ == 0) && spinel_framer_.get()) {
      spinel_framer_->SetInboundAllowanceStatus(false);
    }
  } else {
    zxlogf(ERROR, "OtRadioDevice::HandleRadioRxFrame(): Radio is off\n");
  }
  return ZX_OK;
}

zx_status_t OtRadioDevice::RadioPacketTx(uint8_t* frameBuffer, uint16_t length) {
  zxlogf(INFO, "ot-radio: RadioPacketTx\n");
  zx_port_packet packet = {PORT_KEY_TX_TO_RADIO, ZX_PKT_TYPE_USER, ZX_OK, {}};
  if (!port_.is_valid()) {
    return ZX_ERR_BAD_STATE;
  }
  memcpy(spi_tx_buffer_, frameBuffer, length);
  spi_tx_buffer_len_ = length;
  return port_.queue(&packet);
}

zx_status_t OtRadioDevice::DriverUnitTestGetNCPVersion() {
  spinel_framer_->SetInboundAllowanceStatus(true);
  inbound_allowance_ = kOutboundAllowanceInit;
  return GetNCPVersion();
}

zx_status_t OtRadioDevice::GetNCPVersion() {
  uint8_t get_ncp_version_cmd[] = {0x81, 0x02, 0x02};  // HEADER, CMD ID, PROPERTY ID
  return RadioPacketTx(get_ncp_version_cmd, sizeof(get_ncp_version_cmd));
}

zx_status_t OtRadioDevice::DriverUnitTestGetResetEvent() {
  spinel_framer_->SetInboundAllowanceStatus(true);
  inbound_allowance_ = kOutboundAllowanceInit;
  return Reset();
}

zx_status_t OtRadioDevice::AssertResetPin() {
  zx_status_t status = ZX_OK;
  zxlogf(TRACE, "ot-radio: assert reset pin\n");

  status = gpio_[OT_RADIO_RESET_PIN].Write(0);
  if (status != ZX_OK) {
    zxlogf(ERROR, "ot-radio: gpio write failed\n");
    return status;
  }
  zx::nanosleep(zx::deadline_after(zx::msec(100)));
  return status;
}

zx_status_t OtRadioDevice::Reset() {
  zx_status_t status = ZX_OK;
  zxlogf(TRACE, "ot-radio: reset\n");

  status = gpio_[OT_RADIO_RESET_PIN].Write(0);
  if (status != ZX_OK) {
    zxlogf(ERROR, "ot-radio: gpio write failed\n");
    return status;
  }
  zx::nanosleep(zx::deadline_after(zx::msec(100)));

  status = gpio_[OT_RADIO_RESET_PIN].Write(1);
  if (status != ZX_OK) {
    zxlogf(ERROR, "ot-radio: gpio write failed\n");
    return status;
  }
  zx::nanosleep(zx::deadline_after(zx::msec(400)));
  return status;
}

zx_status_t OtRadioDevice::RadioThread() {
  zx_status_t status = ZX_OK;
  zxlogf(ERROR, "ot-radio: entered thread\n");

  while (true) {
    zx_port_packet_t packet = {};
    int timeout_ms = spinel_framer_->GetTimeoutMs();
    auto status = port_.wait(zx::deadline_after(zx::msec(timeout_ms)), &packet);

    if (status == ZX_ERR_TIMED_OUT) {
      spinel_framer_->TrySpiTransaction();
      ReadRadioPacket();
      continue;
    } else if (status != ZX_OK) {
      zxlogf(ERROR, "ot-radio: port wait failed: %d\n", status);
      return thrd_error;
    }

    if (packet.key == PORT_KEY_EXIT_THREAD) {
      break;
    } else if (packet.key == PORT_KEY_RADIO_IRQ) {
      interrupt_.ack();
      zxlogf(TRACE, "ot-radio: interrupt\n");
      spinel_framer_->HandleInterrupt();
      ReadRadioPacket();
      while (true) {
        uint8_t pin_level = 0;
        gpio_[OT_RADIO_INT_PIN].Read(&pin_level);
        // Interrupt has de-asserted or no more frames can be received.
        if (pin_level != 0 || inbound_allowance_ == 0) {
          break;
        }
        spinel_framer_->HandleInterrupt();
        ReadRadioPacket();
      }
    } else if (packet.key == PORT_KEY_TX_TO_RADIO) {
      spinel_framer_->SendPacketToRadio(spi_tx_buffer_, spi_tx_buffer_len_);
    }
  }
  zxlogf(TRACE, "ot-radio: exiting\n");

  return status;
}

zx_status_t OtRadioDevice::CreateBindAndStart(void* ctx, zx_device_t* parent) {
  std::unique_ptr<OtRadioDevice> ot_radio_dev;
  zx_status_t status = Create(ctx, parent, &ot_radio_dev);
  if (status != ZX_OK) {
    return status;
  }

  status = ot_radio_dev->Bind();
  if (status != ZX_OK) {
    return status;
  }
  // device intentionally leaked as it is now held by DevMgr
  auto dev_ptr = ot_radio_dev.release();

  status = dev_ptr->Start();
  if (status != ZX_OK) {
    return status;
  }

  return status;
}

zx_status_t OtRadioDevice::Create(void* ctx, zx_device_t* parent,
                                  std::unique_ptr<OtRadioDevice>* out) {
  auto dev = std::make_unique<OtRadioDevice>(parent);
  zx_status_t status = dev->Init();

  if (status != ZX_OK) {
    zxlogf(ERROR, "ot-radio: Driver init failed %d\n", status);
    return status;
  }

  *out = std::move(dev);

  return ZX_OK;
}

zx_status_t OtRadioDevice::Bind() {
  zx_status_t status = DdkAdd("ot-radio", 0, nullptr, 0, ZX_PROTOCOL_OT_RADIO);
  if (status != ZX_OK) {
    zxlogf(ERROR, "ot-radio: Could not create device: %d\n", status);
    return status;
  } else {
    zxlogf(TRACE, "ot-radio: Added device\n");
  }
  return status;
}

zx_status_t OtRadioDevice::CreateAndBindPortToIntr() {
  zx_status_t status = zx::port::create(ZX_PORT_BIND_TO_INTERRUPT, &port_);
  if (status != ZX_OK) {
    zxlogf(ERROR, "ot-radio: port create failed %d\n", status);
    return status;
  }

  status = interrupt_.bind(port_, PORT_KEY_RADIO_IRQ, 0);
  if (status != ZX_OK) {
    zxlogf(ERROR, "ot-radio: interrupt bind failed %d\n", status);
    return status;
  }

  return ZX_OK;
}

void OtRadioDevice::StartRadioThread() {
  auto callback = [](void* cookie) {
    return reinterpret_cast<OtRadioDevice*>(cookie)->RadioThread();
  };
  int ret = thrd_create_with_name(&thread_, callback, this, "ot-radio-thread");
  ZX_DEBUG_ASSERT(ret == thrd_success);

  // Set status flag so shutdown can take appropriate action
  // cleared by StopRadioThread
  thrd_status_.radio_thrd_running = true;
}

zx_status_t OtRadioDevice::Start() {
  zx_status_t status = CreateAndBindPortToIntr();
  if (status != ZX_OK) {
    return status;
  }

  StartRadioThread();
  auto cleanup = fbl::MakeAutoCall([&]() { ShutDown(); });

#ifdef INTERNAL_ACCESS
  // Update the NCP Firmware if new version is available
  bool update_fw = false;
  status = CheckFWUpdateRequired(&update_fw);
  if (status != ZX_OK) {
    zxlogf(ERROR, "ot-radio: Check FW Update required failed with status: %d\n", status);
    return status;
  }

  if (update_fw) {
    // Print as it may be useful, expected to be rare occurence,
    zxlogf(INFO, "ot-radio: Will start FW update\n");

    // Stop the loop for handling port events, so port can be used by bootloader
    StopRadioThread();

    // Update firmware here
    OtRadioDeviceBootloader dev_bl(this);
    OtRadioBlResult result = dev_bl.UpdateRadioFirmware();
    if (result != BL_RET_SUCCESS) {
      zxlogf(ERROR, "ot-radio: radio firmware update failed with %d. Last zx_status %d\n", result,
             dev_bl.GetLastZxStatus());
      return ZX_ERR_INTERNAL;
    }

    zxlogf(INFO, "ot-radio: FW update done successfully\n");

    // Restart the Radio thread:
    StartRadioThread();
  } else {
    zxlogf(TRACE, "ot-radio: NCP firmware is already up-to-date\n");
  }
#endif

  status = StartLoopThread();
  if (status != ZX_OK) {
    zxlogf(ERROR, "ot-radio: Could not start loop thread\n");
    return status;
  }

  zxlogf(TRACE, "ot-radio: Started thread\n");

  cleanup.cancel();

  return status;
}

#ifdef INTERNAL_ACCESS
zx_status_t OtRadioDevice::CheckFWUpdateRequired(bool* update_fw) {
  // TODO - Early exit is added until License issue on firmware is resolved and
  // we can upload the actual firmware. Tracked by bug 43881
  // Once license is available:
  // 1) Create new CIPD package with firmware and upload it.
  // 2) Checkin integration change which uses that package
  // 3) Remove next two lines and also enable corresponding tests
  *update_fw = false;
  return ZX_OK;

  // Get the new firmware version:
  std::string new_fw_version = GetNewFirmwareVersion();
  if (new_fw_version.size() == 0) {
    // Invalid version string indicates invalid firmware
    zxlogf(ERROR, "ot-radio: The new firmware is invalid\n");
    *update_fw = false;
    // Return error instead of ZX_OK and just not-updating,
    // may point to some bug
    return ZX_ERR_NO_RESOURCES;
  }

  // Get current firmware version
  std::string cur_fw_version;
  auto status = GetNCPVersion();
  if (status != ZX_OK) {
    zxlogf(ERROR, "ot-radio: get ncp version failed with status: %d\n", status);
    return status;
  }

  // Wait for response to arrive, signaled by spi_rx_complete_
  status = sync_completion_wait(&spi_rx_complete_, ZX_SEC(10));
  if (status != ZX_OK) {
    zxlogf(ERROR,
           "ot-radio: sync_completion_wait_deadline failed with status: %d, \
       this means firmware may be behaving incorrectly\n",
           status);
    *update_fw = true;
    return ZX_OK;
  }

  // Response is received, copy it to the string cur_fw_version
  // First make sure that last character is null
  ZX_DEBUG_ASSERT(spi_rx_buffer_len_ <= kMaxFrameSize);
  spi_rx_buffer_[spi_rx_buffer_len_ - 1] = '\0';
  zxlogf(TRACE, "ot-radio: response received size = %d, value : %s\n", spi_rx_buffer_len_,
         reinterpret_cast<char*>(&(spi_rx_buffer_[3])));
  cur_fw_version.assign(reinterpret_cast<char*>(&(spi_rx_buffer_[3])));

  // We want to update firmware if the versions don't match
  *update_fw = (cur_fw_version.compare(new_fw_version) != 0);
  return ZX_OK;
}
#endif

void OtRadioDevice::DdkRelease() { delete this; }

void OtRadioDevice::DdkUnbindNew(ddk::UnbindTxn txn) {
  ShutDown();
  txn.Reply();
}

void OtRadioDevice::StopRadioThread() {
  if (thrd_status_.radio_thrd_running) {
    zx_port_packet packet = {PORT_KEY_EXIT_THREAD, ZX_PKT_TYPE_USER, ZX_OK, {}};
    port_.queue(&packet);
    thrd_join(thread_, NULL);
    thrd_status_.radio_thrd_running = false;
  }
}

void OtRadioDevice::StopLoopThread() {
  if (thrd_status_.loop_thrd_running) {
    loop_.Shutdown();
    thrd_status_.loop_thrd_running = false;
  }
}

zx_status_t OtRadioDevice::ShutDown() {
  StopRadioThread();

  gpio_[OT_RADIO_INT_PIN].ReleaseInterrupt();
  interrupt_.destroy();

  StopLoopThread();

  return ZX_OK;
}

static constexpr zx_driver_ops_t driver_ops = []() {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = OtRadioDevice::CreateBindAndStart;
  ops.run_unit_tests = OtRadioDevice::RunUnitTests;
  return ops;
}();

}  // namespace ot

// clang-format off
ZIRCON_DRIVER_BEGIN(ot, ot::driver_ops, "ot_radio", "0.1", 3)
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_COMPOSITE),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, PDEV_VID_GENERIC),
    BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_DID, PDEV_DID_OT_RADIO),
ZIRCON_DRIVER_END(ot)
    // clang-format on
