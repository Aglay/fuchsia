// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/lib/fidl_codec/display_handle.h"

#include <zircon/system/public/zircon/features.h>
#include <zircon/system/public/zircon/rights.h>
#include <zircon/system/public/zircon/syscalls/exception.h>
#include <zircon/system/public/zircon/syscalls/object.h>
#include <zircon/system/public/zircon/syscalls/port.h>
#include <zircon/system/public/zircon/syscalls/system.h>
#include <zircon/system/public/zircon/types.h>

#include <iomanip>

namespace fidl_codec {
namespace {

constexpr int kUint32Precision = 8;

}  // namespace

#define ObjTypeNameCase(name) \
  case name:                  \
    printer << #name;         \
    return

void ObjTypeName(zx_obj_type_t obj_type, PrettyPrinter& printer) {
  switch (obj_type) {
    ObjTypeNameCase(ZX_OBJ_TYPE_NONE);
    ObjTypeNameCase(ZX_OBJ_TYPE_PROCESS);
    ObjTypeNameCase(ZX_OBJ_TYPE_THREAD);
    ObjTypeNameCase(ZX_OBJ_TYPE_VMO);
    ObjTypeNameCase(ZX_OBJ_TYPE_CHANNEL);
    ObjTypeNameCase(ZX_OBJ_TYPE_EVENT);
    ObjTypeNameCase(ZX_OBJ_TYPE_PORT);
    ObjTypeNameCase(ZX_OBJ_TYPE_INTERRUPT);
    ObjTypeNameCase(ZX_OBJ_TYPE_PCI_DEVICE);
    ObjTypeNameCase(ZX_OBJ_TYPE_LOG);
    ObjTypeNameCase(ZX_OBJ_TYPE_SOCKET);
    ObjTypeNameCase(ZX_OBJ_TYPE_RESOURCE);
    ObjTypeNameCase(ZX_OBJ_TYPE_EVENTPAIR);
    ObjTypeNameCase(ZX_OBJ_TYPE_JOB);
    ObjTypeNameCase(ZX_OBJ_TYPE_VMAR);
    ObjTypeNameCase(ZX_OBJ_TYPE_FIFO);
    ObjTypeNameCase(ZX_OBJ_TYPE_GUEST);
    ObjTypeNameCase(ZX_OBJ_TYPE_VCPU);
    ObjTypeNameCase(ZX_OBJ_TYPE_TIMER);
    ObjTypeNameCase(ZX_OBJ_TYPE_IOMMU);
    ObjTypeNameCase(ZX_OBJ_TYPE_BTI);
    ObjTypeNameCase(ZX_OBJ_TYPE_PROFILE);
    ObjTypeNameCase(ZX_OBJ_TYPE_PMT);
    ObjTypeNameCase(ZX_OBJ_TYPE_SUSPEND_TOKEN);
    ObjTypeNameCase(ZX_OBJ_TYPE_PAGER);
    ObjTypeNameCase(ZX_OBJ_TYPE_EXCEPTION);
    default:
      printer << obj_type;
      return;
  }
}

#define RightsNameCase(name)       \
  if ((rights & (name)) != 0) {    \
    printer << separator << #name; \
    separator = " | ";             \
  }

void RightsName(zx_rights_t rights, PrettyPrinter& printer) {
  if (rights == 0) {
    printer << "ZX_RIGHT_NONE";
    return;
  }
  const char* separator = "";
  RightsNameCase(ZX_RIGHT_DUPLICATE);
  RightsNameCase(ZX_RIGHT_TRANSFER);
  RightsNameCase(ZX_RIGHT_READ);
  RightsNameCase(ZX_RIGHT_WRITE);
  RightsNameCase(ZX_RIGHT_EXECUTE);
  RightsNameCase(ZX_RIGHT_MAP);
  RightsNameCase(ZX_RIGHT_GET_PROPERTY);
  RightsNameCase(ZX_RIGHT_SET_PROPERTY);
  RightsNameCase(ZX_RIGHT_ENUMERATE);
  RightsNameCase(ZX_RIGHT_DESTROY);
  RightsNameCase(ZX_RIGHT_SET_POLICY);
  RightsNameCase(ZX_RIGHT_GET_POLICY);
  RightsNameCase(ZX_RIGHT_SIGNAL);
  RightsNameCase(ZX_RIGHT_SIGNAL_PEER);
  RightsNameCase(ZX_RIGHT_WAIT);
  RightsNameCase(ZX_RIGHT_INSPECT);
  RightsNameCase(ZX_RIGHT_MANAGE_JOB);
  RightsNameCase(ZX_RIGHT_MANAGE_PROCESS);
  RightsNameCase(ZX_RIGHT_MANAGE_THREAD);
  RightsNameCase(ZX_RIGHT_APPLY_PROFILE);
  RightsNameCase(ZX_RIGHT_SAME_RIGHTS);
}

void DisplayHandle(const zx_handle_info_t& handle, PrettyPrinter& printer) {
  printer << Red;
  if (handle.type != ZX_OBJ_TYPE_NONE) {
    ObjTypeName(handle.type, printer);
    printer << ':';
  }
  char buffer[kUint32Precision + 1];
  snprintf(buffer, sizeof(buffer), "%08x", handle.handle);
  printer << buffer;
  if (handle.rights != 0) {
    printer << Blue << '(';
    RightsName(handle.rights, printer);
    printer << ')';
  }
  printer << ResetColor;
}

}  // namespace fidl_codec
