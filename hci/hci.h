// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <cstdint>

#include <magenta/compiler.h>

#include "apps/bluetooth/common/device_address.h"
#include "apps/bluetooth/hci/hci_constants.h"

// This file contains general opcode/number and static packet definitions for
// the Bluetooth Host-Controller Interface.

namespace bluetooth {
namespace hci {

// HCI opcode as used in command packets.
using OpCode = uint16_t;

// HCI event code as used in event packets.
using EventCode = uint8_t;

// Returns the OGF (OpCode Group Field) which occupies the upper 6-bits of the
// opcode.
inline uint8_t GetOGF(const OpCode opcode) { return opcode >> 10; }

// Returns the OCF (OpCode Command Field) which occupies the lower 10-bits of
// the opcode.
inline uint16_t GetOCF(const OpCode opcode) { return opcode & 0x3FF; }

// Returns the opcode based on the given OGF and OCF fields.
constexpr OpCode DefineOpCode(const uint8_t ogf, const uint16_t ocf) {
  return (static_cast<uint16_t>(ogf & 0x3F) << 10) | (ocf & 0x03FF);
}

// ========================= HCI packet headers ==========================
// NOTE(armansito): The definitions below are incomplete since they get added as
// needed. This list will grow as we support more features.

struct CommandHeader {
  uint16_t opcode;
  uint8_t parameter_total_size;
} __PACKED;

struct EventHeader {
  uint8_t event_code;
  uint8_t parameter_total_size;
} __PACKED;

// ============= HCI Command and Event (op)code and payloads =============

// No-Op
constexpr OpCode kNoOp = 0x0000;

// The following is a list of HCI command and event declarations sorted by OGF
// category. Within each category the commands are sorted by their OCF. Each
// declaration is preceded by the name of the command or event followed by the
// Bluetooth Core Specification version in which it was introduced. Commands
// that apply to a specific Bluetooth sub-technology
// (e.g. BR/EDR, LE, AMP) will also contain that definition.
//
// NOTE(armansito): This list is incomplete. Entries will be added as needed.
// TODO(armansito): Complete the HCI LE commands and events even if we don't
// use them right away..

// ======= Controller & Baseband Commands =======
// Core Spec v5.0 Vol 2, Part E, Section 7.3
constexpr uint8_t kControllerAndBasebandOGF = 0x03;
constexpr OpCode ControllerAndBasebandOpCode(const uint16_t ocf) {
  return DefineOpCode(kControllerAndBasebandOGF, ocf);
}

// ====================
// Reset Command (v1.1)
constexpr OpCode kReset = ControllerAndBasebandOpCode(0x0003);

struct ResetReturnParams {
  // See enum Status in hci_constants.h.
  Status status;
};

// ========================================
// Write Local Name Command (v1.1) (BR/EDR)
constexpr OpCode kWriteLocalName = ControllerAndBasebandOpCode(0x0013);

struct WriteLocalNameCommandParams {
  // A UTF-8 encoded User Friendly Descriptive Name for the device. This can
  // contain up to 248 octets. If the name contained in the parameter is shorter
  // than 248 octets, the end of the name is indicated by a NULL octet (0x00),
  // and the following octets (to fill up 248 octets, which is the length of the
  // parameter) do not have valid values.
  uint8_t local_name[0];
};

struct WriteLocalNameReturnParams {
  // See enum Status in hci_constants.h.
  Status status;
};

// =======================================
// Read Local Name Command (v1.1) (BR/EDR)
constexpr OpCode kReadLocalName = ControllerAndBasebandOpCode(0x0014);

struct ReadLocalNameReturnParams {
  // See enum Status in hci_constants.h.
  Status status;

  // A UTF-8 encoded User Friendly Descriptive Name for the device. This can
  // contain up to 248 octets. If the name contained in the parameter is shorter
  // than 248 octets, the end of the name is indicated by a NULL octet (0x00),
  // and the following octets (to fill up 248 octets, which is the length of the
  // parameter) do not have valid values.
  uint8_t local_name[0];
} __PACKED;

// ============================================
// Read Class of Device Command (v1.1) (BR/EDR)
constexpr OpCode kReadClassOfDevice = ControllerAndBasebandOpCode(0x0023);

struct ReadClassOfDeviceReturnParams {
  // See enum Status in hci_constants.h.
  Status status;

  // Class of Device for the device.
  uint8_t class_of_device[3];
} __PACKED;

// =============================================
// Write Class Of Device Command (v1.1) (BR/EDR)
constexpr OpCode kWriteClassOfDevice = ControllerAndBasebandOpCode(0x0024);

struct WriteClassOfDeviceCommandParams {
  // Class of Device for the device.
  uint8_t class_of_device[3];
};

struct WriteClassOfDeviceReturnParams {
  // See enum Status in hci_constants.h.
  Status status;
};

// =========================================================
// Read Flow Control Mode Command (v3.0 + HS) (BR/EDR & AMP)
constexpr OpCode kReadFlowControlMode = ControllerAndBasebandOpCode(0x0066);

struct ReadFlowControlModeReturnParams {
  // See enum Status in hci_constants.h.
  Status status;

  // The Flow_Control_Mode configuration parameter allows the Host to select the
  // HCI Data flow control mode used by the Controller for ACL Data traffic.
  // See enum class FlowControlMode in hci_constants.h for possible values.
  uint8_t flow_control_mode;
} __PACKED;

// ==========================================================
// Write Flow Control Mode Command (v3.0 + HS) (BR/EDR & AMP)
constexpr OpCode kWriteFlowControlMode = ControllerAndBasebandOpCode(0x0067);

struct WriteFlowControlModeCommandParams {
  // The Flow_Control_Mode configuration parameter allows the Host to select the
  // HCI Data flow control mode used by the Controller for ACL Data traffic.
  // See enum class FlowControlMode in hci_constants.h for possible values.
  uint8_t flow_control_mode;
};

struct WriteFlowControlModeReturnParams {
  // See enum Status in hci_constants.h.
  Status status;
};

// ======= Informational Parameters =======
// Core Spec v5.0 Vol 2, Part E, Section 7.4
constexpr uint8_t kInformationalParamsOGF = 0x04;
constexpr OpCode InformationalParamsOpCode(const uint16_t ocf) {
  return DefineOpCode(kInformationalParamsOGF, ocf);
}

// =============================================
// Read Local Version Information Command (v1.1)
constexpr OpCode kReadLocalVersionInfo = InformationalParamsOpCode(0x0001);

struct ReadLocalVersionInfoReturnParams {
  // See enum Status in hci_constants.h.
  Status status;

  // HCI version (see enum class HCIVersion in hci_constants.h)
  HCIVersion hci_version;

  // Revision of the Current HCI in the BR/EDR Controller.
  uint16_t hci_revision;

  // Version of the Current LMP or PAL in the Controller (see the Bluetooth
  // Assigned Numbers document).
  uint8_t lmp_pal_version;

  // Manufacturer Name of the BR/EDR Controller (see the Bluetooth Assigned
  // Numbers document).
  uint16_t manufacturer_name;

  // Subversion of the Current LMP or PAL in the Controller. This value is
  // implementation dependent.
  uint16_t lmp_pal_subversion;
} __PACKED;

// ============================================
// Read Local Supported Commands Command (v1.2)
constexpr OpCode kReadLocalSupportedCommands =
    InformationalParamsOpCode(0x0002);

struct ReadLocalSupportedCommandsReturnParams {
  // See enum Status in hci_constants.h.
  Status status;

  // 512-bit bitmask for each HCI Command. If a bit is 1, then the Controller
  // supports the corresponding command. See enum class SupportedCommand in
  // hci_constants.h for how to interpret this bitfield.
  uint8_t supported_commands[64];
} __PACKED;

// ============================================
// Read Local Supported Features Command (v1.1)
constexpr OpCode kReadLocalSupportedFeatures =
    InformationalParamsOpCode(0x0003);

struct ReadLocalSupportedFeaturesReturnParams {
  // See enum Status in hci_constants.h.
  Status status;

  // Bit Mask List of LMP features. For details see Core Spec v4.2, Volume 2,
  // Part C, Link Manager Protocol Specification.
  uint8_t lmp_features[8];
} __PACKED;

// ====================================================
// Read Local Extended Features Command (v1.2) (BR/EDR)
constexpr OpCode kReadLocalExtendedFeatures = InformationalParamsOpCode(0x0004);

struct ReadLocalExtendedFeaturesCommandParams {
  // - 0x00: Requests the normal LMP features as returned by
  //   Read_Local_Supported_Features.
  //
  // - 0x01-0xFF: Return the corresponding page of features.
  uint8_t page_number;
};

struct ReadLocalExtendedFeaturesReturnParams {
  // See enum Status in hci_constants.h.
  Status status;

  // - 0x00: The normal LMP features as returned by
  //   Read_Local_Supported_Features.
  //
  // - 0x01-0xFF: The page number of the features returned.
  uint8_t page_number;

  // The highest features page number which contains non-zero bits for the local
  // device.
  uint8_t maximum_page_number;

  // Bit map of requested page of LMP features. See LMP specification for
  // details.
  uint8_t extended_lmp_features[8];
} __PACKED;

// ===============================
// Read Buffer Size Command (v1.1)
constexpr OpCode kReadBufferSize = InformationalParamsOpCode(0x0005);

struct ReadBufferSizeReturnParams {
  // See enum Status in hci_constants.h.
  Status status;

  // Maximum length (in octets) of the data portion of each HCI ACL Data Packet
  // that the Controller is able to accept. This is used to determine the size
  // of the L2CAP segments contained in ACL Data Packets. This excludes the
  // length of the HCI Data packet header.
  uint16_t hc_acl_data_packet_length;

  // Maximum length (in octets) of the data portion of each HCI Synchronous
  // Data Packet that the Controller is able to accept. This excludes the length
  // of the HCI Data packet header.
  uint8_t hc_synchronous_data_packet_length;

  // Total number of HCI ACL Data Packets that can be stored in the data buffers
  // of the Controller.
  uint16_t hc_total_num_acl_data_packets;

  // Total number of HCI Synchronous Data Packets that can be stored in the data
  // buffers of the Controller.
  uint16_t hc_total_num_synchronous_data_packets;
} __PACKED;

// ========================================
// Read BD_ADDR Command (v1.1) (BR/EDR, LE)
constexpr OpCode kReadBDADDR = InformationalParamsOpCode(0x0009);

struct ReadBDADDRReturnParams {
  // See enum Status in hci_constants.h.
  Status status;

  // BD_ADDR of the device.
  common::DeviceAddress bd_addr;
} __PACKED;

// =======================================================
// Read Data Block Size Command (v3.0 + HS) (BR/EDR & AMP)
constexpr OpCode kReadDataBlockSize = InformationalParamsOpCode(0x000A);

struct ReadDataBlockSizeReturnParams {
  // See enum Status in hci_constants.h.
  Status status;

  // Maximum length (in octets) of the data portion of an HCI ACL Data Packet
  // that the Controller is able to accept for transmission. For AMP Controllers
  // this always equals to Max_PDU_Size.
  uint16_t max_acl_data_packet_length;

  // Maximum length (in octets) of the data portion of each HCI ACL Data Packet
  // that the Controller is able to hold in each of its data block buffers.
  uint16_t data_block_length;

  // Total number of data block buffers available in the Controller for the
  // storage of data packets scheduled for transmission.
  uint16_t total_num_data_blocks;
} __PACKED;

// ======= Events =======
// Core Spec v5.0 Vol 2, Part E, Section 7.7

// =============================
// Command Complete Event (v1.1)
constexpr EventCode kCommandCompleteEventCode = 0x0E;

struct CommandCompleteEventParams {
  // The Number of HCI command packets which are allowed to be sent to the
  // Controller from the Host.
  uint8_t num_hci_command_packets;

  // Opcode of the command which caused this event.
  uint16_t command_opcode;

  // This is the return parameter(s) for the command specified in the
  // |command_opcode| event parameter. Refer to the Bluetooth Core Specification
  // v4.2, Vol 2, Part E for each command’s definition for the list of return
  // parameters associated with that command.
  uint8_t return_parameters[0];
} __PACKED;

// ===========================
// Command Status Event (v1.1)
constexpr EventCode kCommandStatusEventCode = 0x0F;
constexpr uint8_t kCommandStatusPending = 0x00;

struct CommandStatusEventParams {
  // See enum Status in hci_constants.h.
  Status status;

  // The Number of HCI command packets which are allowed to be sent to the
  // Controller from the Host.
  uint8_t num_hci_command_packets;

  // Opcode of the command which caused this event and is pending completion.
  uint16_t command_opcode;
} __PACKED;

// ===========================
// Hardware Error Event (v1.1)
constexpr EventCode kHardwareErrorEventCode = 0x10;

struct HardwareErrorEventParams {
  // These Hardware_Codes will be implementation-specific, and can be assigned
  // to indicate various hardware problems.
  uint8_t hardware_code;
};

// ========================================
// Number Of Completed Packets Event (v1.1)
constexpr EventCode kNumberOfCompletedPacketsEventCode = 0x13;

struct NumberOfCompletedPacketsEventData {
  uint16_t connection_handle;
  uint16_t hc_num_of_completed_packets;
} __PACKED;

struct NumberOfCompletedPacketsEventParams {
  // The number of Connection_Handles and Num_HCI_Data_Packets parameters pairs
  // contained in this event.
  uint8_t number_of_handles;

  // Connection handles and the number of HCI Data Packets that have been
  // completed (transmitted or flushed) for the associated connection handle
  // since the previous time the event was returned.
  NumberOfCompletedPacketsEventData data[0];
} __PACKED;

// =========================
// LE Meta Event (v4.0) (LE)
constexpr EventCode kLEMetaEventCode = 0x3E;

struct LEMetaEventParams {
  // The event code for the LE subevent.
  EventCode subevent_code;

  // Beginning of parameters that are specific to the LE subevent.
  uint8_t subevent_parameters[0];
} __PACKED;

// LE Advertising Report Event
constexpr EventCode kLEAdvertisingReportSubeventCode = 0x02;

struct LEAdvertisingReportData {
  // The event type.
  LEAdvertisingEventType event_type;

  // Type of |address| for the advertising device.
  LEAddressType address_type;

  // Public Device Address, Random Device Address, Public Identity Address or
  // Random (static) Identity Address of the advertising device.
  common::DeviceAddress address;

  // Length of the advertising data payload.
  uint8_t length_data;

  // The begining of |length_data| octets of advertising or scan response data
  // formatted as defined in Core Spec v5.0, Vol 3, Part C, Section 11.
  uint8_t data[0];

  // Immediately following |data| there is a single octet field containing the
  // received signal strength for this advertising report. Since |data| has a
  // variable length we do not declare it as a field within this struct.
  //
  //   Range: -127 <= N <= +20
  //   Units: dBm
  //   If N == 127: RSSI is not available.
  //
  // int8_t rssi;
} __PACKED;

struct LEAdvertisingReportSubeventParams {
  // Number of LEAdvertisingReportData instances contained in the array
  // |reports|.
  uint8_t num_reports;

  // Beginning of LEAdvertisingReportData array. Since each report data has a
  // variable length, the contents of |reports| this is declared as an array of
  // uint8_t.
  uint8_t reports[0];
} __PACKED;

// ================================================================
// Number Of Completed Data Blocks Event (v3.0 + HS) (BR/EDR & AMP)
constexpr EventCode kNumberOfCompletedDataBlocksEventCode = 0x48;

struct NumberOfCompletedDataBlocksEventData {
  // Handle (Connection_Handle for a BR/EDR Controller or a Logical_Link_Handle
  // for an AMP Controller).
  uint16_t handle;

  // The number of HCI ACL Data Packets that have been completed (transmitted or
  // flushed) for the associated Handle since the previous time that a Number Of
  // Completed Data Blocks event provided information about this Handle.
  uint16_t num_of_completed_packets;

  // The number of data blocks that have been freed for the associated Handle
  // since the previous time that a Number Of Completed Data Blocks event
  // provided information about this Handle.
  uint16_t num_of_completed_blocks;
} __PACKED;

struct NumberOfCompletedDataBlocksEventParams {
  // This parameter has the following meanings based on its value:
  // - 0x0000: The size of the buffer pool may have changed. The Host is
  //   requested to issue a Read Data Block Size command in order to determine
  //   the new value of Total_Num_Data_Blocks.
  //
  // - 0xXXXX: Total number of data block buffers available in the Controller
  //   for the storage of data packets scheduled for transmission. This
  //   indicates the existing value is unchanged, or increased, or reduced by up
  //   to the sum of the Num_Of_Completed_Blocks values in this command.
  uint16_t total_num_data_blocks;

  // The number of Handles and Num_Of_Completed_Packets and
  // Num_Of_Completed_Blocks parameter triples contained in this event.
  uint8_t number_of_handles;

  NumberOfCompletedDataBlocksEventData data[0];
} __PACKED;

// ======= LE Controller Commands =======
// Core Spec v5.0 Vol 2, Part E, Section 7.8
constexpr uint8_t kLEControllerCommandsOGF = 0x08;
constexpr OpCode LEControllerCommandOpCode(const uint16_t ocf) {
  return DefineOpCode(kLEControllerCommandsOGF, ocf);
}

// =======================================
// LE Read Buffer Size Command (v4.0) (LE)
constexpr OpCode kLEReadBufferSize = LEControllerCommandOpCode(0x0002);

struct LEReadBufferSizeReturnParams {
  // See enum Status in hci_constants.h.
  Status status;

  // Used to determine the size of the L2CAP PDU segments contained in ACL Data
  // Packets, which are transferred from the Host to the Controller to be broken
  // up into packets by the Link Layer. The value of this parameter shall be
  // interpreted as follows:
  //
  // - 0x0000: No dedicated LE Buffer - use Read_Buffer_Size command.
  // - 0x0001-0xFFFF: Maximum length (in octets) of the data portion of each HCI
  //   ACL Data Packet that the Controller is able to accept.
  uint16_t hc_le_acl_data_packet_length;

  // Contains the total number of HCI ACL Data Packets that can be stored in the
  // data buffers of the Controller. The Host determines how the buffers are to
  // be divided between different Connection Handles. The value of this
  // parameter shall be interpreted as follows:
  //
  // - 0x00: No dedicated LE Buffer - use Read_Buffer_Size command.
  // - 0x01-0xFF: Total number of HCI ACL Data Packets that can be stored in the
  //   data buffers of the Controller.
  uint8_t hc_total_num_le_acl_data_packets;
} __PACKED;

// ====================================================
// LE Read Local Supported Features Command (v4.0) (LE)
constexpr OpCode kLEReadLocalSupportedFeatures =
    LEControllerCommandOpCode(0x0003);

struct LEReadLocalSupportedFeaturesReturnParams {
  // See enum Status in hci_constants.h.
  Status status;

  // Bit Mask List of supported LE features. See enum class LEFeatures in
  // hci_constants.h.
  uint8_t le_features[8];
} __PACKED;

struct LEReadMaximumDataLengthReturnParams {
  // See enum Status in hci_constants.h.
  Status status;

  // Maximum number of payload octets that the local Controller supports for
  // transmission of a single Link Layer Data Channel PDU.
  uint16_t supported_max_tx_octets;

  // Maximum time, in microseconds, that the local Controller supports for
  // transmission of a single Link Layer Data Channel PDU.
  uint16_t supported_max_tx_time;

  // Maximum number of payload octets that the local Controller supports for
  // reception of a single Link Layer Data Channel PDU.
  uint16_t supported_max_rx_octets;

  // Maximum time, in microseconds, that the local Controller supports for
  // reception of a single Link Layer Data Channel PDU.
  uint16_t supported_max_rx_time;
} __PACKED;

// =================================================
// LE Set Advertising Parameters Command (v4.0) (LE)
constexpr OpCode kLESetAdvertisingParameters =
    LEControllerCommandOpCode(0x0006);

struct LESetAdvertisingParametersCommandParams {
  // Minimum advertising interval for undirected and low duty cycle directed
  // advertising. This value shall be less than or equal to |adv_interval_max|.
  // |adv_interval_min| and |adv_interval_max| should not be the same value to
  // enable the controller to determine the best advertising interval given
  // other activities.
  //
  //   Range: see kLEAdvertisingInterval[Min|Max] in hci_constants.h
  //   Default: N = kLEAdvertisingIntervalDefault (see hci_constants.h)
  //   Time: N * 0.625 ms
  //   Time Range: 20 ms to 10.24 s
  uint16_t adv_interval_min;

  // Maximum advertising interval for undirected and low duty cycle directed
  // advertising. This value shall be greater than or equal to
  // |adv_interval_max|. |adv_interval_min| and |adv_interval_max| should not be
  // the same value to enable the controller to determine the best advertising
  // interval given other activities.
  //
  //   Range: see kLEAdvertisingInterval[Min|Max] in hci_constants.h
  //   Default: N = kLEAdvertisingIntervalDefault (see hci_constants.h)
  //   Time: N * 0.625 ms
  //   Time Range: 20 ms to 10.24 s
  uint16_t adv_interval_max;

  // Used to determine the packet type that is used for advertising when
  // advertising is enabled (see hci_constants.h)
  LEAdvertisingType adv_type;

  // Indicates the type of address being used in the advertising packets.
  //
  // If |own_address_type| equals 0x02 or 0x03, the |peer_address| parameter
  // contains the peer’s Identity Address and the |peer_address_type| parameter
  // contains the Peer’s Identity Type (i.e. 0x00 or 0x01). These parameters are
  // used to locate the corresponding local IRK in the resolving list; this IRK
  // is used to generate the own address used in the advertisement.
  //
  // If directed advertising is performed, i.e. when Advertising_Type is set to
  // 0x01 (ADV_DIRECT_IND, high duty cycle) or 0x04 (ADV_DIRECT_IND, low duty
  // cycle mode), then the |peer_address_type| and |peer_address| shall be
  // valid.
  //
  // If |own_address_type| equals 0x02 or 0x03, the Controller generates the
  // peer’s Resolvable Private Address using the peer’s IRK corresponding to the
  // peer’s Identity Address contained in the |peer_address| parameter and
  // peer’s Identity Address Type (i.e. 0x00 or 0x01) contained in the
  // |peer_address_type| parameter.
  LEOwnAddressType own_address_type;
  LEPeerAddressType peer_address_type;

  // Public Device Address, Random Device Address, Public Identity Address, or
  // Random (static) Identity Address of the device to be connected.
  common::DeviceAddress peer_address;

  // Bit field that indicates the advertising channels that shall be used when
  // transmitting advertising packets. At least one channel bit shall be set in
  // the |adv_channel_map| parameter (see the constants kLEAdvertisingChannel*
  // in hci_constants.h for possible values).
  uint8_t adv_channel_map;

  // This parameter shall be ignored when directed advertising is enabled (see
  // hci_constants.h for possible values).
  LEAdvFilterPolicy adv_filter_policy;
} __PACKED;

struct LESetAdvertisingParametersReturnParams {
  // See enum Status in hci_constants.h
  Status status;
};

// ===========================================
// LE Set Advertising Data Command (v4.0) (LE)
constexpr OpCode kLESetAdvertisingData = LEControllerCommandOpCode(0x0008);

struct LESetAdvertisingDataCommandParams {
  // The number of significant octets in |adv_data|.
  uint8_t adv_data_length;

  // 31 octets of advertising data formatted as defined in Core Spec v5.0, Vol
  // 3, Part C, Section 11.
  uint8_t adv_data[kMaxLEAdvertisingDataLength];
} __PACKED;

struct LESetAdvertisingDataReturnParams {
  // See enum Status in hci_constants.h
  Status status;
};

// =============================================
// LE Set Advertising Enable Command (v4.0) (LE)
constexpr OpCode kLESetAdvertisingEnable = LEControllerCommandOpCode(0x000A);

struct LESetAdvertisingEnableCommandParams {
  // The LE_Set_Advertising_Enable command is used to request the Controller to
  // start or stop advertising. The Controller manages the timing of
  // advertisements as per the advertising parameters given in the
  // LE_Set_Advertising_Parameters command.
  //
  // The Controller shall continue advertising until the Host issues an
  // LE_Set_Advertising_Enable command with Advertising_Enable set to 0x00
  // (Advertising is disabled) or until a connection is created or until the
  // Advertising is timed out due to high duty cycle Directed Advertising. In
  // these cases, advertising is then disabled.
  GenericEnableParam advertising_enable;
};

struct LESetAdvertisingEnableReturnParams {
  // See enum Status in hci_constants.h.
  Status status;
};

// ==========================================
// LE Set Scan Parameters Command (v4.0) (LE)
constexpr OpCode kLESetScanParameters = LEControllerCommandOpCode(0x000B);

struct LESetScanParametersCommandParams {
  // Controls the type of scan to perform.
  LEScanType scan_type;

  // The LE_Scan_Interval and LE_Scan_Window parameters are recommendations from
  // the Host on how long (LE_Scan_Window) and how frequently (LE_Scan_Interval)
  // the Controller should scan (See Core Spec v5.0, Vol 6, Part B, Section
  // 4.5.3). The LE_Scan_Window parameter shall always be set to a value smaller
  // or equal to the value set for the LE_Scan_Interval parameter. If they are
  // set to the same value scanning should be run continuously.
  //
  //   Range: see kLEScanInterval[Min|Max] in hci_constants.h
  //   Default: N = kLEScanIntervalDefault (see hci_constants.h)
  //   Time: N * 0.625 ms
  //   Time Range: 2.5 ms to 10.24 s
  uint16_t scan_interval;
  uint16_t scan_window;

  // Indicates the type of address being used in the scan request packets (for
  // active scanning).
  LEOwnAddressType own_address_type;

  // The LE white-list and privacy filter policy that should be used while
  // scanning for directed and undirected advertisements.
  LEScanFilterPolicy filter_policy;
} __PACKED;

struct LESetScanParametersReturnParams {
  // See enum status in hci_constants.h
  Status status;
};

// ======================================
// LE Set Scan Enable Command (v4.0) (LE)
constexpr OpCode kLESetScanEnable = LEControllerCommandOpCode(0x000C);

struct LESetScanEnableCommandParams {
  // The LE_Set_Scan_Enable command is used to start scanning. Scanning is used
  // to discover advertising devices nearby.
  //
  // If the LE_Scan_Enable parameter is set to 0x01 and scanning is already
  // enabled, any change to the Filter_Duplicates setting shall take effect.
  // Note: Disabling scanning when it is disabled has no effect.
  GenericEnableParam scanning_enabled;

  // Controls whether the Link Layer should filter out duplicate advertising
  // reports (Filtering_Enabled) to the Host, or if the Link Layer should
  // generate advertising reports for each packet received (Filtering_Disabled).
  // (See Core Spec v5.0, Vol 6, Part B, Section 4.4.3.5)
  GenericEnableParam filter_duplicates;
} __PACKED;

struct LESetScanEnableReturnParams {
  // See enum status in hci_constants.h
  Status status;
};

// ============================================
// LE Read Supported States Command (v4.0) (LE)
constexpr OpCode kLEReadSupportedStates = LEControllerCommandOpCode(0x001C);

struct LEReadSupportedStatesReturnParams {
  // See enum Status in hci_constants.h.
  Status status;

  // Bit-mask of supported state or state combinations. See Core Spec v4.2,
  // Volume 2, Part E, Section 7.8.27 "LE Read Supported States Command".
  uint8_t le_states[8];
} __PACKED;

}  // namespace hci
}  // namespace bluetooth
