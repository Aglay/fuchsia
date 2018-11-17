// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

#include "usb_bus.h"

#include <algorithm>

#include <ddk/debug.h>
#include <ddk/protocol/usb.h>
#include <ddk/usb/usb.h>
#include <lib/zx/time.h>
#include <zircon/compiler.h>
#include <zircon/errors.h>
#include <zircon/hw/usb.h>
#include <zircon/status.h>

namespace wlan {
namespace rtl88xx {
namespace {

// Set to true to log all bus transactions.
constexpr bool kLogBusTransactions = false;

// Register read/write deadline, after which a read or write will fail.
constexpr zx::duration kRegisterIoDeadline = zx::msec(1);

// Returns true iff the given USB interface describes a supported rtl88xx chip's WLAN functionality.
constexpr bool IsRealtekWlanDevice(const usb_interface_descriptor_t& desc) {
    if (desc.bInterfaceClass == 0xFF && desc.bInterfaceSubClass == 0xFF &&
        desc.bInterfaceProtocol == 0xFF) {
        // Prototype board; assume that this is a WLAN interface.
        return true;
    }
    return false;
}

// This class implements the Bus interface over the USB bus.
class UsbBus : public Bus {
   public:
    ~UsbBus() override;

    // Bus implementation.
    Bus::BusType GetBusType() const override;
    zx_status_t ReadRegister(uint16_t offset, uint8_t* value, const char* name) override;
    zx_status_t ReadRegister(uint16_t offset, uint16_t* value, const char* name) override;
    zx_status_t ReadRegister(uint16_t offset, uint32_t* value, const char* name) override;
    zx_status_t WriteRegister(uint16_t offset, uint8_t value, const char* name) override;
    zx_status_t WriteRegister(uint16_t offset, uint16_t value, const char* name) override;
    zx_status_t WriteRegister(uint16_t offset, uint32_t value, const char* name) override;

    // Factory function for UsbBus instances. Returns an instance iff USB initialization is
    // successful.
    static zx_status_t Create(usb_protocol_t* usb_protocol,
                              const usb_interface_descriptor_t& usb_iface_desc,
                              std::unique_ptr<Bus>* bus);

   private:
    UsbBus();
    UsbBus(const UsbBus& other) = delete;
    UsbBus(UsbBus&& other) = delete;
    UsbBus& operator=(UsbBus other) = delete;

    // Implement the register read/write on the USB bus.
    zx_status_t ControlReadRegister(uint16_t offset, char* value, size_t size);
    zx_status_t ControlWriteRegister(uint16_t offset, char* value, size_t size);

    usb_protocol_t usb_protocol_;
};

UsbBus::UsbBus() : usb_protocol_{} {}

UsbBus::~UsbBus() {}

zx_status_t UsbBus::Create(usb_protocol_t* usb_protocol,
                           const usb_interface_descriptor_t& usb_iface_desc,
                           std::unique_ptr<Bus>* bus) {
#if 0   // TODO(sheu): re-enable when Zircon control endpoint stalls are fixed.
    status = usb_set_interface(usb_protocol, usb_iface_desc.bInterfaceNumber,
                               usb_iface_desc.bAlternateSetting);
    if (status != ZX_OK) {
        // usb_set_interface() fails on some Realtek chipsets, with no impact on subsequent
        // functionality.
        zxlogf(TRACE, "rtl88xx: UsbBus::Create() failed to set interface %d alternate %d: %s\n",
               usb_iface_desc.bInterfaceNumber, usb_iface_desc.bAlternateSetting,
               zx_status_get_string(status));
    }
#endif  // 0

    std::unique_ptr<UsbBus> usb_bus(new UsbBus());
    usb_bus->usb_protocol_ = *usb_protocol;

    *bus = std::move(usb_bus);
    return ZX_OK;
}

Bus::BusType UsbBus::GetBusType() const {
    return Bus::BusType::kUsb;
}

zx_status_t UsbBus::ReadRegister(uint16_t offset, uint8_t* value, const char* name) {
    const zx_status_t status =
        ControlReadRegister(offset, reinterpret_cast<char*>(value), sizeof(*value));
    if (status != ZX_OK) {
        zxlogf(ERROR, "rtl88xx: UsbBus::ReadRegister(%s) returned %s\n", name,
               zx_status_get_string(status));
    }
    if (kLogBusTransactions) { zxlogf(INFO, "rtl88xx: UsbBus %-24s  > 0x%02x\n", name, *value); }
    return status;
}

zx_status_t UsbBus::ReadRegister(uint16_t offset, uint16_t* value, const char* name) {
    const zx_status_t status =
        ControlReadRegister(offset, reinterpret_cast<char*>(value), sizeof(*value));
    if (status != ZX_OK) {
        zxlogf(ERROR, "rtl88xx: UsbBus::ReadRegister(%s) returned %s\n", name,
               zx_status_get_string(status));
    }
    if (kLogBusTransactions) { zxlogf(INFO, "rtl88xx: UsbBus %-24s  > 0x%04x\n", name, *value); }
    return status;
}

zx_status_t UsbBus::ReadRegister(uint16_t offset, uint32_t* value, const char* name) {
    const zx_status_t status =
        ControlReadRegister(offset, reinterpret_cast<char*>(value), sizeof(*value));
    if (status != ZX_OK) {
        zxlogf(ERROR, "rtl88xx: UsbBus::ReadRegister(%s) returned %s\n", name,
               zx_status_get_string(status));
    }
    if (kLogBusTransactions) { zxlogf(INFO, "rtl88xx: UsbBus %-24s  > 0x%08x\n", name, *value); }
    return status;
}

zx_status_t UsbBus::WriteRegister(uint16_t offset, uint8_t value, const char* name) {
    const uint8_t original_value = value;
    const zx_status_t status =
        ControlWriteRegister(offset, reinterpret_cast<char*>(&value), sizeof(value));
    if (status != ZX_OK) {
        zxlogf(ERROR, "rtl88xx: UsbBus::WriteRegister(%s) returned %s\n", name,
               zx_status_get_string(status));
    }
    if (kLogBusTransactions) {
        zxlogf(INFO, "rtl88xx: UsbBus %-24s <  0x%02x\n", name, original_value);
    }
    return status;
}

zx_status_t UsbBus::WriteRegister(uint16_t offset, uint16_t value, const char* name) {
    const uint8_t original_value = value;
    const zx_status_t status =
        ControlWriteRegister(offset, reinterpret_cast<char*>(&value), sizeof(value));
    if (status != ZX_OK) {
        zxlogf(ERROR, "rtl88xx: UsbBus::WriteRegister(%s) returned %s\n", name,
               zx_status_get_string(status));
    }
    if (kLogBusTransactions) {
        zxlogf(INFO, "rtl88xx: UsbBus %-24s <  0x%04x\n", name, original_value);
    }
    return status;
}

zx_status_t UsbBus::WriteRegister(uint16_t offset, uint32_t value, const char* name) {
    const uint32_t original_value = value;
    const zx_status_t status =
        ControlWriteRegister(offset, reinterpret_cast<char*>(&value), sizeof(value));
    if (status != ZX_OK) {
        zxlogf(ERROR, "rtl88xx: UsbBus::WriteRegister(%s) returned %s\n", name,
               zx_status_get_string(status));
    }
    if (kLogBusTransactions) {
        zxlogf(INFO, "rtl88xx: UsbBus %-24s <  0x%08x\n", name, original_value);
    }
    return status;
}

zx_status_t UsbBus::ControlReadRegister(uint16_t offset, char* value, size_t size) {
    constexpr uint8_t kRequestType = USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE;
    constexpr uint8_t kRequest = 0x0;
    constexpr uint8_t kIndex = 0x0;

    return usb_control(&usb_protocol_, kRequestType, kRequest, offset, kIndex, value, size,
                       zx::deadline_after(kRegisterIoDeadline).get(), nullptr);
}

zx_status_t UsbBus::ControlWriteRegister(uint16_t offset, char* value, size_t size) {
    constexpr uint8_t kRequestType = USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_DEVICE;
    constexpr uint8_t kRequest = 0x0;
    constexpr uint8_t kIndex = 0x0;

    return usb_control(&usb_protocol_, kRequestType, kRequest, offset, kIndex, value, size,
                       zx::deadline_after(kRegisterIoDeadline).get(), nullptr);
}

}  // namespace

zx_status_t CreateUsbBus(zx_device_t* bus_device, std::unique_ptr<Bus>* bus) {
    zx_status_t status = ZX_OK;

    usb_protocol_t usb_protocol = {};
    status = device_get_protocol(bus_device, ZX_PROTOCOL_USB, &usb_protocol);
    if (status != ZX_OK) {
        // Explicitly do not log an error here. The caller may try with another bus type instead.
        return status;
    }

    usb_device_descriptor_t usb_device_desc;
    usb_get_device_descriptor(&usb_protocol, &usb_device_desc);

    usb_desc_iter_t usb_iter;
    status = usb_desc_iter_init(&usb_protocol, &usb_iter);
    if (status != ZX_OK) {
        zxlogf(ERROR, "rtl88xx: CreateUsbBus() failed to iterate descriptor: %s\n",
               zx_status_get_string(status));
        return status;
    }

    const usb_interface_descriptor_t* usb_iface_desc =
        usb_desc_iter_next_interface(&usb_iter, true);
    while (usb_iface_desc != nullptr) {
        if (IsRealtekWlanDevice(*usb_iface_desc)) { break; }
        usb_iface_desc = usb_desc_iter_next_interface(&usb_iter, true);
    }

    if (usb_iface_desc != nullptr) {
        const uint16_t kLangId = 0;
        uint8_t id_buf[256];
        size_t actual_buflen = 0;
        uint16_t actual_langid = 0;
        if (usb_get_string_descriptor(&usb_protocol, usb_iface_desc->iInterface, kLangId, id_buf,
                                      sizeof(id_buf), &actual_buflen, &actual_langid) != ZX_OK) {
            actual_buflen = 0;
        }
        id_buf[std::min(actual_buflen, countof(id_buf) - 1)] = '\0';
        zxlogf(INFO,
               "rtl88xx: CreateUsbBus() vid=%04x pid=%04x interface=%d alternate=%d class=%d "
               "subclass=%d protocol=%d id=\"%s\"\n",
               usb_device_desc.idVendor, usb_device_desc.idProduct,
               usb_iface_desc->bInterfaceNumber, usb_iface_desc->bAlternateSetting,
               usb_iface_desc->bInterfaceClass, usb_iface_desc->bInterfaceSubClass,
               usb_iface_desc->bInterfaceProtocol, id_buf);

        status = UsbBus::Create(&usb_protocol, *usb_iface_desc, bus);
        if (status != ZX_OK) {
            zxlogf(ERROR, "rtl88xx: UsbBus::Create() returned %s\n", zx_status_get_string(status));
        }
    } else {
        status = ZX_ERR_NOT_SUPPORTED;
        zxlogf(ERROR, "rtl88xx: UsbBus::Create() failed to find interface\n");
    }
    usb_desc_iter_release(&usb_iter);

    return status;
}

}  // namespace rtl88xx
}  // namespace wlan
