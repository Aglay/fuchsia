// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tools/fidlcat/lib/type_decoder.h"

#include <zircon/system/public/zircon/features.h>
#include <zircon/system/public/zircon/rights.h>
#include <zircon/system/public/zircon/syscalls/exception.h>
#include <zircon/system/public/zircon/syscalls/iommu.h>
#include <zircon/system/public/zircon/syscalls/object.h>
#include <zircon/system/public/zircon/syscalls/pci.h>
#include <zircon/system/public/zircon/syscalls/policy.h>
#include <zircon/system/public/zircon/syscalls/port.h>
#include <zircon/system/public/zircon/syscalls/profile.h>
#include <zircon/system/public/zircon/syscalls/system.h>
#include <zircon/system/public/zircon/types.h>

#include <cstdint>
#include <iomanip>
#include <ostream>

namespace fidlcat {

#define BtiPermNameCase(name)      \
  if ((perm & (name)) == (name)) { \
    os << separator << #name;      \
    separator = " | ";             \
  }

void BtiPermName(uint32_t perm, std::ostream& os) {
  if (perm == 0) {
    os << "0";
    return;
  }
  const char* separator = "";
  BtiPermNameCase(ZX_BTI_PERM_READ);
  BtiPermNameCase(ZX_BTI_PERM_WRITE);
  BtiPermNameCase(ZX_BTI_PERM_EXECUTE);
  BtiPermNameCase(ZX_BTI_COMPRESS);
  BtiPermNameCase(ZX_BTI_CONTIGUOUS);
}

#define CachePolicyNameCase(name) \
  case name:                      \
    os << #name;                  \
    return

void CachePolicyName(uint32_t cache_policy, std::ostream& os) {
  switch (cache_policy) {
    CachePolicyNameCase(ZX_CACHE_POLICY_CACHED);
    CachePolicyNameCase(ZX_CACHE_POLICY_UNCACHED);
    CachePolicyNameCase(ZX_CACHE_POLICY_UNCACHED_DEVICE);
    CachePolicyNameCase(ZX_CACHE_POLICY_WRITE_COMBINING);
    default:
      os << cache_policy;
      return;
  }
}

#define ClockNameCase(name) \
  case name:                \
    os << #name;            \
    return

void ClockName(zx_clock_t clock, std::ostream& os) {
  switch (clock) {
    ClockNameCase(ZX_CLOCK_MONOTONIC);
    ClockNameCase(ZX_CLOCK_UTC);
    ClockNameCase(ZX_CLOCK_THREAD);
    default:
      os << clock;
      return;
  }
}

#define ExceptionChannelTypeNameCase(name) \
  case name:                               \
    os << #name;                           \
    return

void ExceptionChannelTypeName(uint32_t type, std::ostream& os) {
  switch (type) {
    ExceptionChannelTypeNameCase(ZX_EXCEPTION_CHANNEL_TYPE_NONE);
    ExceptionChannelTypeNameCase(ZX_EXCEPTION_CHANNEL_TYPE_DEBUGGER);
    ExceptionChannelTypeNameCase(ZX_EXCEPTION_CHANNEL_TYPE_THREAD);
    ExceptionChannelTypeNameCase(ZX_EXCEPTION_CHANNEL_TYPE_PROCESS);
    ExceptionChannelTypeNameCase(ZX_EXCEPTION_CHANNEL_TYPE_JOB);
    ExceptionChannelTypeNameCase(ZX_EXCEPTION_CHANNEL_TYPE_JOB_DEBUGGER);
    default:
      os << static_cast<uint32_t>(type);
      return;
  }
}

#define ExceptionStateNameCase(name) \
  case name:                         \
    os << #name;                     \
    return

void ExceptionStateName(uint32_t state, std::ostream& os) {
  switch (state) {
    ExceptionStateNameCase(ZX_EXCEPTION_STATE_TRY_NEXT);
    ExceptionStateNameCase(ZX_EXCEPTION_STATE_HANDLED);
    default:
      os << static_cast<uint32_t>(state);
      return;
  }
}

#define FeatureKindNameCase(name) \
  case name:                      \
    os << #name;                  \
    return

void FeatureKindName(uint32_t feature_kind, std::ostream& os) {
  switch (feature_kind) {
    FeatureKindNameCase(ZX_FEATURE_KIND_CPU);
    FeatureKindNameCase(ZX_FEATURE_KIND_HW_BREAKPOINT_COUNT);
    FeatureKindNameCase(ZX_FEATURE_KIND_HW_WATCHPOINT_COUNT);
    default:
      os << feature_kind;
      return;
  }
}

#define GuestTrapNameCase(name) \
  case name:                    \
    os << #name;                \
    return

void GuestTrapName(zx_guest_trap_t trap, std::ostream& os) {
  switch (trap) {
    GuestTrapNameCase(ZX_GUEST_TRAP_BELL);
    GuestTrapNameCase(ZX_GUEST_TRAP_MEM);
    GuestTrapNameCase(ZX_GUEST_TRAP_IO);
    default:
      os << trap;
      return;
  }
}

#define InfoMapsTypeCase(name) \
  case name:                   \
    os << #name;               \
    return

void InfoMapsTypeName(zx_info_maps_type_t type, std::ostream& os) {
  switch (type) {
    InfoMapsTypeCase(ZX_INFO_MAPS_TYPE_NONE);
    InfoMapsTypeCase(ZX_INFO_MAPS_TYPE_ASPACE);
    InfoMapsTypeCase(ZX_INFO_MAPS_TYPE_VMAR);
    InfoMapsTypeCase(ZX_INFO_MAPS_TYPE_MAPPING);
    default:
      os << type;
      return;
  }
}

#define InterruptFlagsCase(name) \
  case name:                     \
    os << #name;                 \
    break

#define InterruptFlagsNameFlag(name) \
  if ((flags & (name)) == (name)) {  \
    os << " | " << #name;            \
  }

void InterruptFlagsName(uint32_t flags, std::ostream& os) {
  switch (flags & ZX_INTERRUPT_MODE_MASK) {
    InterruptFlagsCase(ZX_INTERRUPT_MODE_DEFAULT);
    InterruptFlagsCase(ZX_INTERRUPT_MODE_EDGE_LOW);
    InterruptFlagsCase(ZX_INTERRUPT_MODE_EDGE_HIGH);
    InterruptFlagsCase(ZX_INTERRUPT_MODE_LEVEL_LOW);
    InterruptFlagsCase(ZX_INTERRUPT_MODE_LEVEL_HIGH);
    InterruptFlagsCase(ZX_INTERRUPT_MODE_EDGE_BOTH);
    default:
      os << (flags & ZX_INTERRUPT_MODE_MASK);
      break;
  }
  InterruptFlagsNameFlag(ZX_INTERRUPT_REMAP_IRQ);
  InterruptFlagsNameFlag(ZX_INTERRUPT_VIRTUAL);
}

#define IommuTypeNameCase(name) \
  case name:                    \
    os << #name;                \
    return

void IommuTypeName(uint32_t type, std::ostream& os) {
  switch (type) {
    IommuTypeNameCase(ZX_IOMMU_TYPE_DUMMY);
    IommuTypeNameCase(ZX_IOMMU_TYPE_INTEL);
    default:
      os << type;
      return;
  }
}

#define KtraceControlActionNameCase(name) \
  case name:                              \
    os << #name;                          \
    return

void KtraceControlActionName(uint32_t action, std::ostream& os) {
  constexpr uint32_t KTRACE_ACTION_START = 1;
  constexpr uint32_t KTRACE_ACTION_STOP = 2;
  constexpr uint32_t KTRACE_ACTION_REWIND = 3;
  constexpr uint32_t KTRACE_ACTION_NEW_PROBE = 4;
  switch (action) {
    KtraceControlActionNameCase(KTRACE_ACTION_START);
    KtraceControlActionNameCase(KTRACE_ACTION_STOP);
    KtraceControlActionNameCase(KTRACE_ACTION_REWIND);
    KtraceControlActionNameCase(KTRACE_ACTION_NEW_PROBE);
    default:
      os << action;
      return;
  }
}

#define ObjPropsNameCase(name) \
  case name:                   \
    os << #name;               \
    return

void ObjPropsName(zx_obj_props_t obj_props, std::ostream& os) {
  switch (obj_props) {
    ObjPropsNameCase(ZX_OBJ_PROP_NONE);
    ObjPropsNameCase(ZX_OBJ_PROP_WAITABLE);
    default:
      os << obj_props;
      return;
  }
}

#define PacketGuestVcpuTypeNameCase(name) \
  case name:                              \
    os << #name;                          \
    return

void PacketGuestVcpuTypeName(uint8_t type, std::ostream& os) {
  switch (type) {
    PacketGuestVcpuTypeNameCase(ZX_PKT_GUEST_VCPU_INTERRUPT);
    PacketGuestVcpuTypeNameCase(ZX_PKT_GUEST_VCPU_STARTUP);
    default:
      os << static_cast<uint32_t>(type);
      return;
  }
}

#define PacketPageRequestCommandNameCase(name) \
  case name:                                   \
    os << #name;                               \
    return

void PacketPageRequestCommandName(uint16_t command, std::ostream& os) {
  switch (command) {
    PacketPageRequestCommandNameCase(ZX_PAGER_VMO_READ);
    PacketPageRequestCommandNameCase(ZX_PAGER_VMO_COMPLETE);
    default:
      os << static_cast<uint32_t>(command);
      return;
  }
}

#define PciBarTypeNameCase(name) \
  case name:                     \
    os << #name;                 \
    return

void PciBarTypeName(uint32_t type, std::ostream& os) {
  switch (type) {
    PciBarTypeNameCase(ZX_PCI_BAR_TYPE_UNUSED);
    PciBarTypeNameCase(ZX_PCI_BAR_TYPE_MMIO);
    PciBarTypeNameCase(ZX_PCI_BAR_TYPE_PIO);
    default:
      os << static_cast<uint32_t>(type);
      return;
  }
}

#define PolicyNameCase(name) \
  case name:                 \
    os << #name;             \
    return

void PolicyActionName(uint32_t action, std::ostream& os) {
  switch (action) {
    PolicyNameCase(ZX_POL_ACTION_ALLOW);
    PolicyNameCase(ZX_POL_ACTION_DENY);
    PolicyNameCase(ZX_POL_ACTION_ALLOW_EXCEPTION);
    PolicyNameCase(ZX_POL_ACTION_DENY_EXCEPTION);
    PolicyNameCase(ZX_POL_ACTION_KILL);
    default:
      os << action;
      return;
  }
}

void PolicyConditionName(uint32_t condition, std::ostream& os) {
  switch (condition) {
    PolicyNameCase(ZX_POL_BAD_HANDLE);
    PolicyNameCase(ZX_POL_WRONG_OBJECT);
    PolicyNameCase(ZX_POL_VMAR_WX);
    PolicyNameCase(ZX_POL_NEW_ANY);
    PolicyNameCase(ZX_POL_NEW_VMO);
    PolicyNameCase(ZX_POL_NEW_CHANNEL);
    PolicyNameCase(ZX_POL_NEW_EVENT);
    PolicyNameCase(ZX_POL_NEW_EVENTPAIR);
    PolicyNameCase(ZX_POL_NEW_PORT);
    PolicyNameCase(ZX_POL_NEW_SOCKET);
    PolicyNameCase(ZX_POL_NEW_FIFO);
    PolicyNameCase(ZX_POL_NEW_TIMER);
    PolicyNameCase(ZX_POL_NEW_PROCESS);
    PolicyNameCase(ZX_POL_NEW_PROFILE);
    PolicyNameCase(ZX_POL_AMBIENT_MARK_VMO_EXEC);
    default:
      os << condition;
      return;
  }
}

void PolicyTopicName(uint32_t topic, std::ostream& os) {
  switch (topic) {
    PolicyNameCase(ZX_JOB_POL_BASIC);
    PolicyNameCase(ZX_JOB_POL_TIMER_SLACK);
    default:
      os << topic;
      return;
  }
}

constexpr uint32_t kExceptionMask = 0xff;
constexpr int kExceptionNumberShift = 8;
constexpr uint32_t kExceptionNumberMask = 0xff;

#define PortPacketTypeNameCase(name) \
  case name:                         \
    os << #name;                     \
    return

void PortPacketTypeName(uint32_t type, std::ostream& os) {
  switch (type) {
    PortPacketTypeNameCase(ZX_PKT_TYPE_USER);
    PortPacketTypeNameCase(ZX_PKT_TYPE_SIGNAL_ONE);
    PortPacketTypeNameCase(ZX_PKT_TYPE_SIGNAL_REP);
    PortPacketTypeNameCase(ZX_PKT_TYPE_GUEST_BELL);
    PortPacketTypeNameCase(ZX_PKT_TYPE_GUEST_MEM);
    PortPacketTypeNameCase(ZX_PKT_TYPE_GUEST_IO);
    PortPacketTypeNameCase(ZX_PKT_TYPE_GUEST_VCPU);
    PortPacketTypeNameCase(ZX_PKT_TYPE_INTERRUPT);
    PortPacketTypeNameCase(ZX_PKT_TYPE_PAGE_REQUEST);
    default:
      if ((type & kExceptionMask) == ZX_PKT_TYPE_EXCEPTION(0)) {
        os << "ZX_PKT_TYPE_EXCEPTION(" << ((type >> kExceptionNumberShift) & kExceptionNumberMask)
           << ')';
        return;
      }
      os << "port_packet_type=" << type;
      return;
  }
}

#define ProfileInfoFlagsNameCase(name) \
  if ((flags & (name)) == (name)) {    \
    os << separator << #name;          \
    separator = " | ";                 \
  }

void ProfileInfoFlagsName(uint32_t flags, std::ostream& os) {
  if (flags == 0) {
    os << "0";
    return;
  }
  const char* separator = "";
  ProfileInfoFlagsNameCase(ZX_PROFILE_INFO_FLAG_PRIORITY);
  ProfileInfoFlagsNameCase(ZX_PROFILE_INFO_FLAG_CPU_MASK);
}

#define PropTypeNameCase(name) \
  case name:                   \
    os << #name;               \
    return

void PropTypeName(uint32_t type, std::ostream& os) {
  switch (type) {
    PropTypeNameCase(ZX_PROP_NAME);
    PropTypeNameCase(ZX_PROP_REGISTER_FS);
    PropTypeNameCase(ZX_PROP_REGISTER_GS);
    PropTypeNameCase(ZX_PROP_PROCESS_DEBUG_ADDR);
    PropTypeNameCase(ZX_PROP_PROCESS_VDSO_BASE_ADDRESS);
    PropTypeNameCase(ZX_PROP_SOCKET_RX_THRESHOLD);
    PropTypeNameCase(ZX_PROP_SOCKET_TX_THRESHOLD);
    PropTypeNameCase(ZX_PROP_JOB_KILL_ON_OOM);
    PropTypeNameCase(ZX_PROP_EXCEPTION_STATE);
    default:
      os << type;
      return;
  }
}

#define RsrcKindNameCase(name) \
  case name:                   \
    os << #name;               \
    return

void RsrcKindName(zx_rsrc_kind_t kind, std::ostream& os) {
  switch (kind) {
    RsrcKindNameCase(ZX_RSRC_KIND_MMIO);
    RsrcKindNameCase(ZX_RSRC_KIND_IRQ);
    RsrcKindNameCase(ZX_RSRC_KIND_IOPORT);
    RsrcKindNameCase(ZX_RSRC_KIND_HYPERVISOR);
    RsrcKindNameCase(ZX_RSRC_KIND_ROOT);
    RsrcKindNameCase(ZX_RSRC_KIND_VMEX);
    RsrcKindNameCase(ZX_RSRC_KIND_SMC);
    RsrcKindNameCase(ZX_RSRC_KIND_COUNT);
    default:
      os << kind;
      return;
  }
}

#define SignalNameCase(name)          \
  if ((signals & (name)) == (name)) { \
    os << separator << #name;         \
    separator = " | ";                \
  }

void SignalName(zx_signals_t signals, std::ostream& os) {
  if (signals == 0) {
    os << "0";
    return;
  }
  if (signals == __ZX_OBJECT_SIGNAL_ALL) {
    os << "__ZX_OBJECT_SIGNAL_ALL";
    return;
  }
  const char* separator = "";
  SignalNameCase(__ZX_OBJECT_READABLE);
  SignalNameCase(__ZX_OBJECT_WRITABLE);
  SignalNameCase(__ZX_OBJECT_PEER_CLOSED);
  SignalNameCase(__ZX_OBJECT_SIGNALED);
  SignalNameCase(__ZX_OBJECT_SIGNAL_4);
  SignalNameCase(__ZX_OBJECT_SIGNAL_5);
  SignalNameCase(__ZX_OBJECT_SIGNAL_6);
  SignalNameCase(__ZX_OBJECT_SIGNAL_7);
  SignalNameCase(__ZX_OBJECT_SIGNAL_8);
  SignalNameCase(__ZX_OBJECT_SIGNAL_9);
  SignalNameCase(__ZX_OBJECT_SIGNAL_10);
  SignalNameCase(__ZX_OBJECT_SIGNAL_11);
  SignalNameCase(__ZX_OBJECT_SIGNAL_12);
  SignalNameCase(__ZX_OBJECT_SIGNAL_13);
  SignalNameCase(__ZX_OBJECT_SIGNAL_14);
  SignalNameCase(__ZX_OBJECT_SIGNAL_15);
  SignalNameCase(__ZX_OBJECT_SIGNAL_16);
  SignalNameCase(__ZX_OBJECT_SIGNAL_17);
  SignalNameCase(__ZX_OBJECT_SIGNAL_18);
  SignalNameCase(__ZX_OBJECT_SIGNAL_19);
  SignalNameCase(__ZX_OBJECT_SIGNAL_20);
  SignalNameCase(__ZX_OBJECT_SIGNAL_21);
  SignalNameCase(__ZX_OBJECT_SIGNAL_22);
  SignalNameCase(__ZX_OBJECT_HANDLE_CLOSED);
  SignalNameCase(ZX_USER_SIGNAL_0);
  SignalNameCase(ZX_USER_SIGNAL_1);
  SignalNameCase(ZX_USER_SIGNAL_2);
  SignalNameCase(ZX_USER_SIGNAL_3);
  SignalNameCase(ZX_USER_SIGNAL_4);
  SignalNameCase(ZX_USER_SIGNAL_5);
  SignalNameCase(ZX_USER_SIGNAL_6);
  SignalNameCase(ZX_USER_SIGNAL_7);
}

#define SocketCreateOptionsNameCase(name) \
  case name:                              \
    os << #name;                          \
    return

void SocketCreateOptionsName(uint32_t options, std::ostream& os) {
  switch (options) {
    SocketCreateOptionsNameCase(ZX_SOCKET_STREAM);
    SocketCreateOptionsNameCase(ZX_SOCKET_DATAGRAM);
    default:
      os << static_cast<uint32_t>(options);
      return;
  }
}

#define SocketReadOptionsNameCase(name) \
  case name:                            \
    os << #name;                        \
    return

void SocketReadOptionsName(uint32_t options, std::ostream& os) {
  switch (options) {
    SocketReadOptionsNameCase(ZX_SOCKET_PEEK);
    default:
      os << static_cast<uint32_t>(options);
      return;
  }
}

#define SocketShutdownOptionsNameCase(name) \
  if ((options & (name)) == (name)) {       \
    os << separator << #name;               \
    separator = " | ";                      \
  }

void SocketShutdownOptionsName(uint32_t options, std::ostream& os) {
  if (options == 0) {
    os << "0";
    return;
  }
  const char* separator = "";
  SocketShutdownOptionsNameCase(ZX_SOCKET_SHUTDOWN_WRITE);
  SocketShutdownOptionsNameCase(ZX_SOCKET_SHUTDOWN_READ);
}

#define StatusNameCase(name) \
  case name:                 \
    os << #name;             \
    return

void StatusName(zx_status_t status, std::ostream& os) {
  switch (status) {
    StatusNameCase(ZX_OK);
    StatusNameCase(ZX_ERR_INTERNAL);
    StatusNameCase(ZX_ERR_NOT_SUPPORTED);
    StatusNameCase(ZX_ERR_NO_RESOURCES);
    StatusNameCase(ZX_ERR_NO_MEMORY);
    StatusNameCase(ZX_ERR_INTERNAL_INTR_RETRY);
    StatusNameCase(ZX_ERR_INVALID_ARGS);
    StatusNameCase(ZX_ERR_BAD_HANDLE);
    StatusNameCase(ZX_ERR_WRONG_TYPE);
    StatusNameCase(ZX_ERR_BAD_SYSCALL);
    StatusNameCase(ZX_ERR_OUT_OF_RANGE);
    StatusNameCase(ZX_ERR_BUFFER_TOO_SMALL);
    StatusNameCase(ZX_ERR_BAD_STATE);
    StatusNameCase(ZX_ERR_TIMED_OUT);
    StatusNameCase(ZX_ERR_SHOULD_WAIT);
    StatusNameCase(ZX_ERR_CANCELED);
    StatusNameCase(ZX_ERR_PEER_CLOSED);
    StatusNameCase(ZX_ERR_NOT_FOUND);
    StatusNameCase(ZX_ERR_ALREADY_EXISTS);
    StatusNameCase(ZX_ERR_ALREADY_BOUND);
    StatusNameCase(ZX_ERR_UNAVAILABLE);
    StatusNameCase(ZX_ERR_ACCESS_DENIED);
    StatusNameCase(ZX_ERR_IO);
    StatusNameCase(ZX_ERR_IO_REFUSED);
    StatusNameCase(ZX_ERR_IO_DATA_INTEGRITY);
    StatusNameCase(ZX_ERR_IO_DATA_LOSS);
    StatusNameCase(ZX_ERR_IO_NOT_PRESENT);
    StatusNameCase(ZX_ERR_IO_OVERRUN);
    StatusNameCase(ZX_ERR_IO_MISSED_DEADLINE);
    StatusNameCase(ZX_ERR_IO_INVALID);
    StatusNameCase(ZX_ERR_BAD_PATH);
    StatusNameCase(ZX_ERR_NOT_DIR);
    StatusNameCase(ZX_ERR_NOT_FILE);
    StatusNameCase(ZX_ERR_FILE_BIG);
    StatusNameCase(ZX_ERR_NO_SPACE);
    StatusNameCase(ZX_ERR_NOT_EMPTY);
    StatusNameCase(ZX_ERR_STOP);
    StatusNameCase(ZX_ERR_NEXT);
    StatusNameCase(ZX_ERR_ASYNC);
    StatusNameCase(ZX_ERR_PROTOCOL_NOT_SUPPORTED);
    StatusNameCase(ZX_ERR_ADDRESS_UNREACHABLE);
    StatusNameCase(ZX_ERR_ADDRESS_IN_USE);
    StatusNameCase(ZX_ERR_NOT_CONNECTED);
    StatusNameCase(ZX_ERR_CONNECTION_REFUSED);
    StatusNameCase(ZX_ERR_CONNECTION_RESET);
    StatusNameCase(ZX_ERR_CONNECTION_ABORTED);
    default:
      os << "status=" << status;
      return;
  }
}

void StatusName(const fidl_codec::Colors& colors, zx_status_t status, std::ostream& os) {
  if (status == ZX_OK) {
    os << colors.green;
  } else {
    os << colors.red;
  }
  StatusName(status, os);
  os << colors.reset;
}

#define SystemEventTypeNameCase(name) \
  case name:                          \
    os << #name;                      \
    return

void SystemEventTypeName(zx_system_event_type_t type, std::ostream& os) {
  switch (type) {
    SystemEventTypeNameCase(ZX_SYSTEM_EVENT_LOW_MEMORY);
    default:
      os << type;
      return;
  }
}

#define SystemPowerctlNameCase(name) \
  case name:                         \
    os << #name;                     \
    return

void SystemPowerctlName(uint32_t powerctl, std::ostream& os) {
  switch (powerctl) {
    SystemPowerctlNameCase(ZX_SYSTEM_POWERCTL_ENABLE_ALL_CPUS);
    SystemPowerctlNameCase(ZX_SYSTEM_POWERCTL_DISABLE_ALL_CPUS_BUT_PRIMARY);
    SystemPowerctlNameCase(ZX_SYSTEM_POWERCTL_ACPI_TRANSITION_S_STATE);
    SystemPowerctlNameCase(ZX_SYSTEM_POWERCTL_X86_SET_PKG_PL1);
    SystemPowerctlNameCase(ZX_SYSTEM_POWERCTL_REBOOT);
    SystemPowerctlNameCase(ZX_SYSTEM_POWERCTL_REBOOT_BOOTLOADER);
    SystemPowerctlNameCase(ZX_SYSTEM_POWERCTL_REBOOT_RECOVERY);
    SystemPowerctlNameCase(ZX_SYSTEM_POWERCTL_SHUTDOWN);
    default:
      os << powerctl;
      return;
  }
}

#define ThreadStateNameCase(name) \
  case name:                      \
    os << #name;                  \
    return

void ThreadStateName(uint32_t state, std::ostream& os) {
  switch (state) {
    ThreadStateNameCase(ZX_THREAD_STATE_NEW);
    ThreadStateNameCase(ZX_THREAD_STATE_RUNNING);
    ThreadStateNameCase(ZX_THREAD_STATE_SUSPENDED);
    ThreadStateNameCase(ZX_THREAD_STATE_BLOCKED);
    ThreadStateNameCase(ZX_THREAD_STATE_DYING);
    ThreadStateNameCase(ZX_THREAD_STATE_DEAD);
    ThreadStateNameCase(ZX_THREAD_STATE_BLOCKED_EXCEPTION);
    ThreadStateNameCase(ZX_THREAD_STATE_BLOCKED_SLEEPING);
    ThreadStateNameCase(ZX_THREAD_STATE_BLOCKED_FUTEX);
    ThreadStateNameCase(ZX_THREAD_STATE_BLOCKED_PORT);
    ThreadStateNameCase(ZX_THREAD_STATE_BLOCKED_CHANNEL);
    ThreadStateNameCase(ZX_THREAD_STATE_BLOCKED_WAIT_ONE);
    ThreadStateNameCase(ZX_THREAD_STATE_BLOCKED_WAIT_MANY);
    ThreadStateNameCase(ZX_THREAD_STATE_BLOCKED_INTERRUPT);
    ThreadStateNameCase(ZX_THREAD_STATE_BLOCKED_PAGER);
    default:
      os << static_cast<uint32_t>(state);
      return;
  }
}

#define ThreadStateTopicNameCase(name) \
  case name:                           \
    os << #name;                       \
    return

void ThreadStateTopicName(zx_thread_state_topic_t topic, std::ostream& os) {
  switch (topic) {
    ThreadStateTopicNameCase(ZX_THREAD_STATE_GENERAL_REGS);
    ThreadStateTopicNameCase(ZX_THREAD_STATE_FP_REGS);
    ThreadStateTopicNameCase(ZX_THREAD_STATE_VECTOR_REGS);
    ThreadStateTopicNameCase(ZX_THREAD_STATE_DEBUG_REGS);
    ThreadStateTopicNameCase(ZX_THREAD_STATE_SINGLE_STEP);
    ThreadStateTopicNameCase(ZX_THREAD_X86_REGISTER_FS);
    ThreadStateTopicNameCase(ZX_THREAD_X86_REGISTER_GS);
    default:
      os << static_cast<uint32_t>(topic);
      return;
  }
}

#define TimerOptionNameCase(name) \
  case name:                      \
    os << #name;                  \
    return

void TimerOptionName(uint32_t option, std::ostream& os) {
  switch (option) {
    TimerOptionNameCase(ZX_TIMER_SLACK_CENTER);
    TimerOptionNameCase(ZX_TIMER_SLACK_EARLY);
    TimerOptionNameCase(ZX_TIMER_SLACK_LATE);
    default:
      os << option;
      return;
  }
}

#define TopicNameCase(name) \
  case name:                \
    os << #name;            \
    return

void TopicName(uint32_t topic, std::ostream& os) {
  switch (topic) {
    TopicNameCase(ZX_INFO_NONE);
    TopicNameCase(ZX_INFO_HANDLE_VALID);
    TopicNameCase(ZX_INFO_HANDLE_BASIC);
    TopicNameCase(ZX_INFO_PROCESS);
    TopicNameCase(ZX_INFO_PROCESS_THREADS);
    TopicNameCase(ZX_INFO_VMAR);
    TopicNameCase(ZX_INFO_JOB_CHILDREN);
    TopicNameCase(ZX_INFO_JOB_PROCESSES);
    TopicNameCase(ZX_INFO_THREAD);
    TopicNameCase(ZX_INFO_THREAD_EXCEPTION_REPORT);
    TopicNameCase(ZX_INFO_TASK_STATS);
    TopicNameCase(ZX_INFO_PROCESS_MAPS);
    TopicNameCase(ZX_INFO_PROCESS_VMOS);
    TopicNameCase(ZX_INFO_THREAD_STATS);
    TopicNameCase(ZX_INFO_CPU_STATS);
    TopicNameCase(ZX_INFO_KMEM_STATS);
    TopicNameCase(ZX_INFO_RESOURCE);
    TopicNameCase(ZX_INFO_HANDLE_COUNT);
    TopicNameCase(ZX_INFO_BTI);
    TopicNameCase(ZX_INFO_PROCESS_HANDLE_STATS);
    TopicNameCase(ZX_INFO_SOCKET);
    TopicNameCase(ZX_INFO_VMO);
    TopicNameCase(ZX_INFO_JOB);
    default:
      os << "topic=" << topic;
      return;
  }
}

#define VcpuNameCase(name) \
  case name:               \
    os << #name;           \
    return

void VcpuName(uint32_t type, std::ostream& os) {
  switch (type) {
    VcpuNameCase(ZX_VCPU_STATE);
    VcpuNameCase(ZX_VCPU_IO);
    default:
      os << type;
      return;
  }
}

#define VmOptionAlign(name) \
  case name:                \
    os << #name;            \
    separator = " | ";      \
    break;

#define VmOptionCase(name)           \
  if ((option & (name)) == (name)) { \
    os << separator << #name;        \
    separator = " | ";               \
  }

void VmOptionName(zx_vm_option_t option, std::ostream& os) {
  if (option == 0) {
    os << "0";
    return;
  }
  const char* separator = "";
  switch (option & ~((1 << ZX_VM_ALIGN_BASE) - 1)) {
    VmOptionAlign(ZX_VM_ALIGN_1KB);
    VmOptionAlign(ZX_VM_ALIGN_2KB);
    VmOptionAlign(ZX_VM_ALIGN_4KB);
    VmOptionAlign(ZX_VM_ALIGN_8KB);
    VmOptionAlign(ZX_VM_ALIGN_16KB);
    VmOptionAlign(ZX_VM_ALIGN_32KB);
    VmOptionAlign(ZX_VM_ALIGN_64KB);
    VmOptionAlign(ZX_VM_ALIGN_128KB);
    VmOptionAlign(ZX_VM_ALIGN_256KB);
    VmOptionAlign(ZX_VM_ALIGN_512KB);
    VmOptionAlign(ZX_VM_ALIGN_1MB);
    VmOptionAlign(ZX_VM_ALIGN_2MB);
    VmOptionAlign(ZX_VM_ALIGN_4MB);
    VmOptionAlign(ZX_VM_ALIGN_8MB);
    VmOptionAlign(ZX_VM_ALIGN_16MB);
    VmOptionAlign(ZX_VM_ALIGN_32MB);
    VmOptionAlign(ZX_VM_ALIGN_64MB);
    VmOptionAlign(ZX_VM_ALIGN_128MB);
    VmOptionAlign(ZX_VM_ALIGN_256MB);
    VmOptionAlign(ZX_VM_ALIGN_512MB);
    VmOptionAlign(ZX_VM_ALIGN_1GB);
    VmOptionAlign(ZX_VM_ALIGN_2GB);
    VmOptionAlign(ZX_VM_ALIGN_4GB);
    default:
      if ((option >> ZX_VM_ALIGN_BASE) != 0) {
        os << (option >> ZX_VM_ALIGN_BASE);
      }
      break;
  }
  VmOptionCase(ZX_VM_PERM_READ);
  VmOptionCase(ZX_VM_PERM_WRITE);
  VmOptionCase(ZX_VM_PERM_EXECUTE);
  VmOptionCase(ZX_VM_COMPACT);
  VmOptionCase(ZX_VM_SPECIFIC);
  VmOptionCase(ZX_VM_SPECIFIC_OVERWRITE);
  VmOptionCase(ZX_VM_CAN_MAP_SPECIFIC);
  VmOptionCase(ZX_VM_CAN_MAP_READ);
  VmOptionCase(ZX_VM_CAN_MAP_WRITE);
  VmOptionCase(ZX_VM_CAN_MAP_EXECUTE);
  VmOptionCase(ZX_VM_MAP_RANGE);
  VmOptionCase(ZX_VM_REQUIRE_NON_RESIZABLE);
  VmOptionCase(ZX_VM_ALLOW_FAULTS);
}

#define VmoCreationOptionNameCase(name) \
  if ((options & (name)) == (name)) {   \
    os << separator << #name;           \
    separator = " | ";                  \
  }

void VmoCreationOptionName(uint32_t options, std::ostream& os) {
  if (options == 0) {
    os << "0";
    return;
  }
  const char* separator = "";
  VmoCreationOptionNameCase(ZX_VMO_RESIZABLE);
}

#define VmoOpNameCase(name) \
  case name:                \
    os << #name;            \
    return

void VmoOpName(uint32_t op, std::ostream& os) {
  switch (op) {
    VmoOpNameCase(ZX_VMO_OP_COMMIT);
    VmoOpNameCase(ZX_VMO_OP_DECOMMIT);
    VmoOpNameCase(ZX_VMO_OP_LOCK);
    VmoOpNameCase(ZX_VMO_OP_UNLOCK);
    VmoOpNameCase(ZX_VMO_OP_CACHE_SYNC);
    VmoOpNameCase(ZX_VMO_OP_CACHE_INVALIDATE);
    VmoOpNameCase(ZX_VMO_OP_CACHE_CLEAN);
    VmoOpNameCase(ZX_VMO_OP_CACHE_CLEAN_INVALIDATE);
    default:
      os << op;
      return;
  }
}

#define VmoOptionNameCase(name)       \
  if ((options & (name)) == (name)) { \
    os << separator << #name;         \
    separator = " | ";                \
  }

void VmoOptionName(uint32_t options, std::ostream& os) {
  if (options == 0) {
    os << "0";
    return;
  }
  const char* separator = "";
  VmoOptionNameCase(ZX_VMO_CHILD_COPY_ON_WRITE);
  VmoOptionNameCase(ZX_VMO_CHILD_RESIZABLE);
  VmoOptionNameCase(ZX_VMO_CHILD_SLICE);
  VmoOptionNameCase(ZX_VMO_CHILD_PRIVATE_PAGER_COPY);
}

#define VmoTypeNameCase(name)      \
  if ((type & (name)) == (name)) { \
    os << " | " << #name;          \
  }

void VmoTypeName(uint32_t type, std::ostream& os) {
  if ((type & 1) == ZX_INFO_VMO_TYPE_PHYSICAL) {
    os << "ZX_INFO_VMO_TYPE_PHYSICAL";
  } else {
    os << "ZX_INFO_VMO_TYPE_PAGED";
  }
  VmoTypeNameCase(ZX_INFO_VMO_RESIZABLE);
  VmoTypeNameCase(ZX_INFO_VMO_IS_COW_CLONE);
  VmoTypeNameCase(ZX_INFO_VMO_VIA_HANDLE);
  VmoTypeNameCase(ZX_INFO_VMO_VIA_MAPPING);
  VmoTypeNameCase(ZX_INFO_VMO_PAGER_BACKED);
  VmoTypeNameCase(ZX_INFO_VMO_CONTIGUOUS);
}

void DisplayType(const fidl_codec::Colors& colors, SyscallType type, std::ostream& os) {
  switch (type) {
    case SyscallType::kBool:
      os << ":" << colors.green << "bool" << colors.reset << ": ";
      break;
    case SyscallType::kChar:
      os << ":" << colors.green << "char" << colors.reset << ": ";
      break;
    case SyscallType::kCharArray:
      os << ":" << colors.green << "char[]" << colors.reset << ": ";
      break;
    case SyscallType::kInt32:
      os << ":" << colors.green << "int32" << colors.reset << ": ";
      break;
    case SyscallType::kInt64:
      os << ":" << colors.green << "int64" << colors.reset << ": ";
      break;
    case SyscallType::kUint8:
    case SyscallType::kUint8Hexa:
      os << ":" << colors.green << "uint8" << colors.reset << ": ";
      break;
    case SyscallType::kUint8ArrayDecimal:
    case SyscallType::kUint8ArrayHexa:
      os << ":" << colors.green << "uint8[]" << colors.reset << ": ";
      break;
    case SyscallType::kUint16:
    case SyscallType::kUint16Hexa:
      os << ":" << colors.green << "uint16" << colors.reset << ": ";
      break;
    case SyscallType::kUint16ArrayDecimal:
    case SyscallType::kUint16ArrayHexa:
      os << ":" << colors.green << "uint16[]" << colors.reset << ": ";
      break;
    case SyscallType::kUint32:
    case SyscallType::kUint32Hexa:
      os << ":" << colors.green << "uint32" << colors.reset << ": ";
      break;
    case SyscallType::kUint32ArrayDecimal:
    case SyscallType::kUint32ArrayHexa:
      os << ":" << colors.green << "uint32[]" << colors.reset << ": ";
      break;
    case SyscallType::kUint64:
    case SyscallType::kUint64Hexa:
      os << ":" << colors.green << "uint64" << colors.reset << ": ";
      break;
    case SyscallType::kUint64ArrayDecimal:
    case SyscallType::kUint64ArrayHexa:
      os << ":" << colors.green << "uint64[]" << colors.reset << ": ";
      break;
    case SyscallType::kUint128Hexa:
      os << ":" << colors.green << "uint128" << colors.reset << ": ";
      break;
    case SyscallType::kUint128ArrayHexa:
      os << ":" << colors.green << "uint128[]" << colors.reset << ": ";
      break;
    case SyscallType::kBtiPerm:
      os << ":" << colors.green << "zx_bti_perm_t" << colors.reset << ": ";
      break;
    case SyscallType::kCachePolicy:
      os << ":" << colors.green << "zx_cache_policy_t" << colors.reset << ": ";
      break;
    case SyscallType::kClock:
      os << ":" << colors.green << "clock" << colors.reset << ": ";
      break;
    case SyscallType::kDuration:
      os << ":" << colors.green << "duration" << colors.reset << ": ";
      break;
    case SyscallType::kExceptionChannelType:
      os << ":" << colors.green << "zx_info_thread_t::wait_exception_channel_type" << colors.reset
         << ": ";
      break;
    case SyscallType::kExceptionState:
      os << ":" << colors.green << "zx_exception_state_t" << colors.reset << ": ";
      break;
    case SyscallType::kFeatureKind:
      os << ":" << colors.green << "zx_feature_kind_t" << colors.reset << ": ";
      break;
    case SyscallType::kFutex:
      os << ":" << colors.green << "zx_futex_t" << colors.reset << ": ";
      break;
    case SyscallType::kGpAddr:
      os << ":" << colors.green << "zx_gpaddr_t" << colors.reset << ": ";
      break;
    case SyscallType::kGuestTrap:
      os << ":" << colors.green << "zx_guest_trap_t" << colors.reset << ": ";
      break;
    case SyscallType::kHandle:
      os << ":" << colors.green << "handle" << colors.reset << ": ";
      break;
    case SyscallType::kInfoMapsType:
      os << ":" << colors.green << "zx_info_maps_type_t" << colors.reset << ": ";
      break;
    case SyscallType::kInterruptFlags:
      os << ":" << colors.green << "zx_interrupt_flags_t" << colors.reset << ": ";
      break;
    case SyscallType::kIommuType:
      os << ":" << colors.green << "zx_iommu_type_t" << colors.reset << ": ";
      break;
    case SyscallType::kKoid:
      os << ":" << colors.green << "zx_koid_t" << colors.reset << ": ";
      break;
    case SyscallType::kKtraceControlAction:
      os << ":" << colors.green << "zx_ktrace_control_action_t" << colors.reset << ": ";
      break;
    case SyscallType::kMonotonicTime:
      os << ":" << colors.green << "zx_time_t" << colors.reset << ": ";
      break;
    case SyscallType::kObjectInfoTopic:
      os << ":" << colors.green << "zx_object_info_topic_t" << colors.reset << ": ";
      break;
    case SyscallType::kObjProps:
      os << ":" << colors.green << "zx_obj_props_t" << colors.reset << ": ";
      break;
    case SyscallType::kObjType:
      os << ":" << colors.green << "zx_obj_type_t" << colors.reset << ": ";
      break;
    case SyscallType::kPacketGuestVcpuType:
      os << ":" << colors.green << "zx_packet_guest_vcpu_t::type" << colors.reset << ": ";
      break;
    case SyscallType::kPacketPageRequestCommand:
      os << ":" << colors.green << "zx_packet_page_request_t::command" << colors.reset << ": ";
      break;
    case SyscallType::kPaddr:
      os << ":" << colors.green << "zx_paddr_t" << colors.reset << ": ";
      break;
    case SyscallType::kPciBarType:
      os << ":" << colors.green << "zx_pci_bar_type_t" << colors.reset << ": ";
      break;
    case SyscallType::kPolicyAction:
      os << ":" << colors.green << "zx_policy_action_t" << colors.reset << ": ";
      break;
    case SyscallType::kPolicyCondition:
      os << ":" << colors.green << "zx_policy_condition_t" << colors.reset << ": ";
      break;
    case SyscallType::kPolicyTopic:
      os << ":" << colors.green << "zx_policy_topic_t" << colors.reset << ": ";
      break;
    case SyscallType::kPortPacketType:
      os << ":" << colors.green << "zx_port_packet_t::type" << colors.reset << ": ";
      break;
    case SyscallType::kProfileInfoFlags:
      os << ":" << colors.green << "zx_profile_info_flags_t" << colors.reset << ": ";
      break;
    case SyscallType::kPropType:
      os << ":" << colors.green << "zx_prop_type_t" << colors.reset << ": ";
      break;
    case SyscallType::kRights:
      os << ":" << colors.green << "zx_rights_t" << colors.reset << ": ";
      break;
    case SyscallType::kRsrcKind:
      os << ":" << colors.green << "zx_rsrc_kind_t" << colors.reset << ": ";
      break;
    case SyscallType::kSignals:
      os << ":" << colors.green << "signals" << colors.reset << ": ";
      break;
    case SyscallType::kSize:
      os << ":" << colors.green << "size_t" << colors.reset << ": ";
      break;
    case SyscallType::kSocketCreateOptions:
      os << ":" << colors.green << "zx_socket_create_options_t" << colors.reset << ": ";
      break;
    case SyscallType::kSocketReadOptions:
      os << ":" << colors.green << "zx_socket_read_options_t" << colors.reset << ": ";
      break;
    case SyscallType::kSocketShutdownOptions:
      os << ":" << colors.green << "zx_socket_shutdown_options_t" << colors.reset << ": ";
      break;
    case SyscallType::kStatus:
      os << ":" << colors.green << "status_t" << colors.reset << ": ";
      break;
    case SyscallType::kSystemEventType:
      os << ":" << colors.green << "zx_system_event_type_t" << colors.reset << ": ";
      break;
    case SyscallType::kSystemPowerctl:
      os << ":" << colors.green << "zx_system_powerctl_t" << colors.reset << ": ";
      break;
    case SyscallType::kThreadState:
      os << ":" << colors.green << "zx_info_thread_t::state" << colors.reset << ": ";
      break;
    case SyscallType::kThreadStateTopic:
      os << ":" << colors.green << "zx_thread_state_topic_t" << colors.reset << ": ";
      break;
    case SyscallType::kTime:
      os << ":" << colors.green << "time" << colors.reset << ": ";
      break;
    case SyscallType::kTimerOption:
      os << ":" << colors.green << "zx_timer_option_t" << colors.reset << ": ";
      break;
    case SyscallType::kUintptr:
      os << ":" << colors.green << "uintptr_t" << colors.reset << ": ";
      break;
    case SyscallType::kVaddr:
      os << ":" << colors.green << "zx_vaddr_t" << colors.reset << ": ";
      break;
    case SyscallType::kVcpu:
      os << ":" << colors.green << "zx_vcpu_t" << colors.reset << ": ";
      break;
    case SyscallType::kVmOption:
      os << ":" << colors.green << "zx_vm_option_t" << colors.reset << ": ";
      break;
    case SyscallType::kVmoCreationOption:
      os << ":" << colors.green << "zx_vmo_creation_option_t" << colors.reset << ": ";
      break;
    case SyscallType::kVmoOp:
      os << ":" << colors.green << "zx_vmo_op_t" << colors.reset << ": ";
      break;
    case SyscallType::kVmoOption:
      os << ":" << colors.green << "zx_vmo_option_t" << colors.reset << ": ";
      break;
    case SyscallType::kVmoType:
      os << ":" << colors.green << "zx_info_vmo_type_t" << colors.reset << ": ";
      break;
    case SyscallType::kStruct:
      break;
  }
}

}  // namespace fidlcat
